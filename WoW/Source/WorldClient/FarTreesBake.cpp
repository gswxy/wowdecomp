// FARTREES: not in 12340. See FarTreesBake.h and fartrees/FARTREES-PLAN.md.
#include "WoW/Source/WorldClient/FarTreesBake.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Engine/Source/BlizzardCore/SFile2-Core.h"

namespace FarTreesBake {

namespace {

constexpr float kMapC = 32.0f * 533.33333f;
constexpr uint32_t kCacheVersion = 3;   // bump when the picture code changes: every cache file re-bakes (3: wxl 4-view billboards)
constexpr float kTreeMinHeight = 6.0f;   // yd as placed (wraithhost-lite FarLod.hpp): smaller trees never get a card

// ---------------------------------------------------------------- queue

std::thread s_thread;
std::mutex s_lock;
std::condition_variable s_wake;
std::atomic<bool> s_stop{ false };
struct Job { bool tile; std::string a; int32_t x, y; };
std::deque<Job> s_jobs;
std::deque<TileResult> s_tiles;
std::deque<ModelResult> s_models;

// ---------------------------------------------------------------- files

bool ReadFile(const std::string& name, std::vector<uint8_t>& out) {
    SFileImpl* file = nullptr;
    if (!SFileOpenFile(name.c_str(), &file) || !file) return false;
    out.resize(SFileGetFileSize(file, nullptr));
    const bool ok = !out.empty() && SFileReadFile(file, out.data(), static_cast<uint32_t>(out.size()), nullptr, nullptr, 0);
    SFileCloseFile(file);
    return ok;
}

uint64_t Fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

template <class T> T Get(const std::vector<uint8_t>& d, size_t o) {
    T v{};
    if (o <= d.size() && sizeof(T) <= d.size() - o) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

std::string Lower(std::string s) {
    for (char& c : s) { if (c == '/') c = '\\'; else if (c >= 'A' && c <= 'Z') c = char(c + 32); }
    return s;
}

// wraithhost-lite FarLod.cpp ModelInfo, on the whole path ("...\\TREES\\...", "PineTree04", "Palm..."): the
// night-elf and Felwood tree-sized bushes too; fallen leaves, stumps, logs and roots are not (a giant's own
// "TreeRoots" and the leafless "NoLeaves" trees are). Plus the tree-named props the stock listfile has.
bool IsTree(const std::string& lowerPath) {
    auto has = [&](const char* w) { return lowerPath.find(w) != std::string::npos; };
    const bool treeish = has("tree") || has("palm") || has("kalidarbush") || has("felwoodbush");
    if (!treeish || has("street") || (has("leaves") && !has("noleaves")) || has("stump") || has("log")
        || (has("root") && !has("treeroot")))
        return false;
    for (const char* w : { "banner", "hut", "bridge", "dock", "cloak", "spine" })   // not wxl: props named after trees
        if (has(w)) return false;
    return true;
}

// Per file name (the leaf: the game shares one CM2Shared per leaf, M2Cache.cpp, so the first path seen is the model
// for every placement of that name): the path, and its header box if it passes FarLod's 0.5 - 500 yd test.
struct Header { std::string path; bool tree = false; float height = 0, size = 0; };
std::unordered_map<std::string, Header> s_headers;   // worker thread only

const Header& HeaderOf(const std::string& model) {
    const std::string leaf = model.substr(model.find_last_of('\\') + 1);
    auto it = s_headers.find(leaf);
    if (it != s_headers.end()) return it->second;
    Header& h = s_headers[leaf];
    h.path = model;
    std::vector<uint8_t> b;
    if (IsTree(model) && ReadFile(model, b) && b.size() >= 0xBC && !std::memcmp(b.data(), "MD20", 4)) {
        float box[6];
        for (int k = 0; k < 6; ++k) box[k] = Get<float>(b, 0xA0 + 4 * k);
        h.height = box[5] - box[2];
        const float width = std::max(box[3] - box[0], box[4] - box[1]);
        h.size = std::max(h.height, width);
        h.tree = h.height > 0.5f && width > 0.1f && h.height <= 500.0f;
    }
    return h;
}

// ---------------------------------------------------------------- ADT

void ScanTile(const std::string& map, int32_t tx, int32_t ty, TileResult& r) {
    r.map = map; r.tx = tx; r.ty = ty;
    char name[300];
    std::snprintf(name, sizeof(name), "World\\Maps\\%s\\%s_%d_%d.adt", map.c_str(), map.c_str(), tx, ty);
    std::vector<uint8_t> d;
    if (!ReadFile(name, d)) return;
    size_t mmdx = 0, mmdxN = 0, mmid = 0, mmidN = 0, mddf = 0, mddfN = 0;
    for (size_t o = 0; o + 8 <= d.size();) {
        const uint32_t tag = Get<uint32_t>(d, o), size = Get<uint32_t>(d, o + 4);
        if (size > d.size() - o - 8) break;   // broken chunk: also no endless loop on a size that wraps o
        if (tag == 'MMDX') { mmdx = o + 8; mmdxN = size; }
        else if (tag == 'MMID') { mmid = o + 8; mmidN = size / 4; }
        else if (tag == 'MDDF') { mddf = o + 8; mddfN = size / 36; }
        o += 8 + size;
    }
    for (size_t k = 0; k < mddfN; ++k) {
        const size_t e = mddf + 36 * k;
        const uint32_t nid = Get<uint32_t>(d, e);
        if (nid >= mmidN) continue;
        const uint32_t off = Get<uint32_t>(d, mmid + 4 * nid);
        if (off >= mmdxN) continue;
        const char* str = reinterpret_cast<const char*>(d.data() + mmdx + off);
        std::string model = Lower(std::string(str, strnlen(str, mmdxN - off)));
        const size_t dot = model.rfind('.');
        if (dot != std::string::npos) model = model.substr(0, dot) + ".m2";
        const Header& h = HeaderOf(model);
        const float scale = Get<uint16_t>(d, e + 32) / 1024.0f;
        if (!h.tree || h.height * scale < kTreeMinHeight) continue;
        Place p;
        p.model = h.path;
        p.size = h.size;
        p.uniqueId = Get<uint32_t>(d, e + 4);
        p.x = kMapC - Get<float>(d, e + 16);   // as CMap::CreateDoodadDef
        p.y = kMapC - Get<float>(d, e + 8);
        p.z = Get<float>(d, e + 12);
        float yaw = (Get<float>(d, e + 24) + 180.0f) / 360.0f;   // RotateAroundZ(rot.y + pi)
        p.yaw = yaw - std::floor(yaw);
        p.scale = scale;
        p.rot[0] = Get<float>(d, e + 20); p.rot[1] = Get<float>(d, e + 24); p.rot[2] = Get<float>(d, e + 28);
        r.places.push_back(p);
    }
}

// ---------------------------------------------------------------- textures (BLP2 decode, DXT5 encode)

struct Image { uint32_t w = 1, h = 1; std::vector<uint8_t> rgba = std::vector<uint8_t>(4, 128); };

void Rgb565(uint16_t c, uint8_t* o) {
    o[0] = uint8_t(((c >> 11) & 31) * 255 / 31); o[1] = uint8_t(((c >> 5) & 63) * 255 / 63); o[2] = uint8_t((c & 31) * 255 / 31);
}

void DecodeBlock(const uint8_t* b, int alphaEnc, uint8_t out[16][4]) {
    const uint8_t* cb = alphaEnc == 0 ? b : b + 8;
    const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = 255; pal[3][3] = 255;
    for (int i = 0; i < 3; ++i) {
        if (c0 > c1 || alphaEnc != 0) { pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3); pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3); }
        else { pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2); pal[3][i] = 0; }
    }
    if (c0 <= c1 && alphaEnc == 0) pal[3][3] = 0;
    const uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
    if (alphaEnc == 1) {
        for (int i = 0; i < 16; ++i) out[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else if (alphaEnc == 7) {
        uint8_t a[8] = { b[0], b[1] };
        for (int i = 2; i < 8; ++i)
            a[i] = b[0] > b[1] ? uint8_t(((8 - i) * b[0] + (i - 1) * b[1]) / 7) : (i < 6 ? uint8_t(((6 - i) * b[0] + (i - 1) * b[1]) / 5) : (i == 6 ? 0 : 255));
        uint64_t ab = 0;
        for (int i = 0; i < 6; ++i) ab |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) out[i][3] = a[(ab >> (3 * i)) & 7];
    }
}

// The level of a BLP2 nearest 256 px wide, as RGBA8. False for anything else (BLP1, broken files).
bool DecodeBlp(const std::vector<uint8_t>& d, Image& img) {
    if (d.size() < 1172 || std::memcmp(d.data(), "BLP2", 4)) return false;
    const uint8_t enc = d[8], alphaDepth = d[9], alphaEnc = d[10];
    uint32_t w = Get<uint32_t>(d, 12), h = Get<uint32_t>(d, 16), level = 0;
    while (w > 256 && level < 15 && Get<uint32_t>(d, 20 + 4 * (level + 1)) && Get<uint32_t>(d, 84 + 4 * (level + 1))) { w = std::max(w >> 1, 1u); h = std::max(h >> 1, 1u); ++level; }
    const uint32_t off = Get<uint32_t>(d, 20 + 4 * level), size = Get<uint32_t>(d, 84 + 4 * level);
    if (!off || uint64_t(off) + size > d.size() || !w || !h || w > 8192 || h > 8192) return false;
    const uint8_t* p = d.data() + off;
    img.w = w; img.h = h;
    img.rgba.assign(size_t(w) * h * 4, 255);
    if (enc == 2) {
        const uint32_t bs = alphaEnc == 0 ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (size_t(bw) * bh * bs > size) return false;
        uint8_t px[16][4];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                DecodeBlock(p + (by * bw + bx) * bs, alphaDepth == 0 && alphaEnc == 0 ? 0 : alphaEnc, px);
                if (alphaDepth == 0) for (auto& q : px) q[3] = 255;
                for (int i = 0; i < 16; ++i) {
                    const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) { uint8_t* o = &img.rgba[(y * w + x) * 4]; o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2]; o[3] = px[i][3]; }
                }
            }
        return true;
    }
    if (enc == 1) {   // palette (BGRA) + alpha of alphaDepth bits
        const size_t n = size_t(w) * h;
        if (n + (alphaDepth == 1 || alphaDepth == 4 || alphaDepth == 8 ? (n * alphaDepth + 7) / 8 : 0) > size) return false;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* c = d.data() + 148 + 4 * p[i];
            uint8_t* o = &img.rgba[i * 4];
            o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
            if (alphaDepth == 8) o[3] = p[n + i];
            else if (alphaDepth == 1) o[3] = (p[n + i / 8] >> (i & 7)) & 1 ? 255 : 0;
            else if (alphaDepth == 4) o[3] = uint8_t(((p[n + i / 2] >> (4 * (i & 1))) & 15) * 17);
        }
        return true;
    }
    if (enc == 3 && size_t(w) * h * 4 <= size) {   // BGRA
        for (size_t i = 0; i < size_t(w) * h; ++i) { uint8_t* o = &img.rgba[i * 4]; o[0] = p[i * 4 + 2]; o[1] = p[i * 4 + 1]; o[2] = p[i * 4]; o[3] = p[i * 4 + 3]; }
        return true;
    }
    return false;
}

uint16_t To565(const float* c) {
    auto q = [](float v, int m) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * m)); };
    return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

// DXT5 of a w x h RGBA float image (w, h >= 1; padded to 4 x 4 blocks).
std::vector<uint8_t> EncodeDxt5(const std::vector<float>& img, uint32_t w, uint32_t h) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> out(size_t(bw) * bh * 16);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            float px[16][4];
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = std::min(bx * 4 + (i & 3), w - 1), y = std::min(by * 4 + (i >> 2), h - 1);
                std::memcpy(px[i], &img[(size_t(y) * w + x) * 4], 16);
            }
            uint8_t* b = &out[(size_t(by) * bw + bx) * 16];
            // Alpha: 8-value mode between the block's max and min.
            float amin = 1, amax = 0;
            for (auto& q : px) { amin = std::min(amin, q[3]); amax = std::max(amax, q[3]); }
            const uint8_t a0 = uint8_t(std::lround(amax * 255)), a1 = uint8_t(std::lround(amin * 255));
            b[0] = a0; b[1] = a1;
            uint64_t ab = 0;
            for (int i = 0; i < 16; ++i) {
                int idx = 0;
                if (a0 > a1) {
                    const float t = (a0 - px[i][3] * 255) / float(a0 - a1);   // 0 at a0, 1 at a1
                    const int s = std::clamp(int(std::lround(t * 7)), 0, 7);
                    idx = s == 0 ? 0 : (s == 7 ? 1 : s + 1);
                }
                ab |= uint64_t(idx) << (3 * i);
            }
            for (int i = 0; i < 6; ++i) b[2 + i] = uint8_t(ab >> (8 * i));
            // Colour: the box corners, 4-colour mode.
            float lo[3] = { 1, 1, 1 }, hi[3] = { 0, 0, 0 };
            for (auto& q : px) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], q[k]); hi[k] = std::max(hi[k], q[k]); }
            uint16_t c0 = To565(hi), c1 = To565(lo);
            if (c0 < c1) std::swap(c0, c1);
            uint8_t e[4][3];
            Rgb565(c0, e[0]); Rgb565(c1, e[1]);
            for (int k = 0; k < 3; ++k) { e[2][k] = uint8_t((2 * e[0][k] + e[1][k]) / 3); e[3][k] = uint8_t((e[0][k] + 2 * e[1][k]) / 3); }
            uint32_t bits = 0;
            if (c0 != c1)
                for (int i = 0; i < 16; ++i) {
                    int best = 0; float bd = 1e9f;
                    for (int j = 0; j < 4; ++j) {
                        float dd = 0;
                        for (int k = 0; k < 3; ++k) { const float t = px[i][k] * 255 - e[j][k]; dd += t * t; }
                        if (dd < bd) { bd = dd; best = j; }
                    }
                    bits |= uint32_t(best) << (2 * i);
                }
            b[8] = uint8_t(c0); b[9] = uint8_t(c0 >> 8); b[10] = uint8_t(c1); b[11] = uint8_t(c1 >> 8);
            std::memcpy(b + 12, &bits, 4);
        }
    return out;
}

// ---------------------------------------------------------------- model
// A port of wraithhost-lite Impostor.cpp Render (the wxl-pack far trees): skin-00 rest pose, kViews orthographic side
// views every 90 deg about z, rastered at 2 x kPx and boxed down, the textures at their <= 256 px level, batch colour
// x transparency at frame 0, the two-texture combiners, the game's alpha keys.

uint32_t Rd32(const std::vector<uint8_t>& d, size_t o) { return Get<uint32_t>(d, o); }
uint16_t Rd16(const std::vector<uint8_t>& d, size_t o) { return Get<uint16_t>(d, o); }
float RdF(const std::vector<uint8_t>& d, size_t o) { return Get<float>(d, o); }

// Impostor.cpp BatchColour: rgb from the colour track, alpha = its alpha x the transparency, frame 0 of the first
// animation; white where it has none. b: the 24-byte skin batch.
void BatchColour(const std::vector<uint8_t>& m, const std::vector<uint8_t>& s, size_t b, float rgba[4]) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f;
    if (m.size() < 0xBC) return;
    auto frame0 = [&](uint32_t at, uint32_t comps, float* v) {
        if (uint64_t(at) + 20 > m.size() || !Rd32(m, at + 12)) return;
        const uint32_t o = Rd32(m, at + 16);
        if (uint64_t(o) + 8 > m.size() || !Rd32(m, o)) return;
        const uint32_t o2 = Rd32(m, o + 4);
        if (uint64_t(o2) + comps * 4 > m.size()) return;
        if (comps == 3) for (uint32_t k = 0; k < 3; ++k) v[k] = std::clamp(RdF(m, o2 + k * 4), 0.0f, 1.0f);
        else v[0] = std::clamp(float(int16_t(Rd16(m, o2))) / 32767.0f, 0.0f, 1.0f);   // fixed16
    };
    const int16_t colour = int16_t(Rd16(s, b + 8));
    if (colour >= 0 && uint32_t(colour) < Rd32(m, 0x48)) {
        const uint32_t at = Rd32(m, 0x4C) + uint32_t(colour) * 0x28;   // M2Color: rgb track, alpha track
        frame0(at, 3, rgba);
        frame0(at + 20, 1, rgba + 3);
    }
    const uint32_t weight = Rd16(s, b + 20);
    if (weight < Rd32(m, 0x90) && uint64_t(Rd32(m, 0x94)) + (weight + 1) * 2 <= m.size()) {
        const uint32_t t = Rd16(m, Rd32(m, 0x94) + weight * 2);
        float tr = 1.0f;
        if (t < Rd32(m, 0x58)) frame0(Rd32(m, 0x5C) + t * 20, 1, &tr);
        rgba[3] *= tr;
    }
}

enum : uint8_t { kOne = 0, kMul, kMul2, kAdd };
enum : uint8_t { kAlphaT1 = 0, kAlphaT2, kAlphaT1T2, kAlphaT1PlusT2 };
struct Layer {
    uint32_t start = 0, count = 0, blend = 0; bool unlit = false; const Image* tex = nullptr; float colour[4];
    const Image* tex2 = nullptr; uint8_t op2 = kOne, uv2 = 0, alpha2 = kAlphaT1; bool alphaX2 = false;
};
struct Mesh {
    std::vector<uint8_t> m2, skin;
    std::vector<Layer> layers;                 // blend 0-2 (Impostor.cpp layers[0]; the glow layers are left out)
    std::deque<Image> textures;                // stable addresses for Layer::tex
    float box[6] = {};
    uint64_t fingerprint = 0;
};

// decode false: only the files the fingerprint covers are read (a cache hit needs no pictures).
bool LoadMesh(const std::string& path, Mesh& M, bool decode) {
    std::vector<uint8_t>& m = M.m2;
    std::vector<uint8_t>& s = M.skin;
    if (!ReadFile(path, m) || m.size() < 0x108 || std::memcmp(m.data(), "MD20", 4)) return false;
    if (!ReadFile(path.substr(0, path.size() - 3) + "00.skin", s) || s.size() < 48 || std::memcmp(s.data(), "SKIN", 4)) return false;
    M.fingerprint = Fnv(s.data(), s.size(), Fnv(m.data(), m.size(), Fnv(&kCacheVersion, 4)));
    for (int k = 0; k < 6; ++k) M.box[k] = RdF(m, 0xA0 + 4 * k);
    auto arr = [&](const std::vector<uint8_t>& d, uint32_t at, uint32_t stride, uint32_t& n) -> uint32_t {
        n = Rd32(d, at);
        const uint32_t ofs = Rd32(d, at + 4);
        if (!n || uint64_t(ofs) + uint64_t(n) * stride > d.size()) { n = 0; return 0; }
        return ofs;
    };
    uint32_t nVert = 0, nTex = 0, nMat = 0, nLookup = 0, nSub = 0, nBatch = 0, nCombos = 0, nCoord = 0;
    arr(m, 0x3C, 48, nVert);
    const uint32_t texs = arr(m, 0x50, 16, nTex), mats = arr(m, 0x70, 4, nMat), lookup = arr(m, 0x80, 2, nLookup);
    const uint32_t subs = arr(s, 0x1C, 48, nSub), batches = arr(s, 0x24, 24, nBatch);
    const uint32_t combos = (Rd32(m, 0x10) & 0x8) && m.size() >= 0x138 ? arr(m, 0x130, 2, nCombos) : 0;
    const uint32_t coords = arr(m, 0x88, 2, nCoord);
    if (!nVert || !nSub || !nBatch) return false;

    // Textures by lookup slot, read once (every read one goes in the fingerprint), decoded at most 256 px across.
    std::unordered_map<uint32_t, const Image*> byTex;
    auto texture = [&](uint32_t combo) -> const Image* {
        if (!nLookup || combo >= nLookup) return nullptr;
        const uint32_t t = Rd16(m, lookup + combo * 2);
        auto it = byTex.find(t);
        if (it != byTex.end()) return it->second;
        const Image* got = nullptr;
        if (t < nTex) {
            const size_t e = texs + t * 16;
            const uint32_t type = Rd32(m, e), len = Rd32(m, e + 8), at = Rd32(m, e + 12);
            std::vector<uint8_t> blp;
            if (type == 0 && len >= 2 && uint64_t(at) + len <= m.size()
                && ReadFile(std::string(reinterpret_cast<const char*>(&m[at]), strnlen(reinterpret_cast<const char*>(&m[at]), len)), blp)) {
                M.fingerprint = Fnv(blp.data(), blp.size(), M.fingerprint);
                M.textures.emplace_back();
                if (!decode || DecodeBlp(blp, M.textures.back())) got = &M.textures.back();
            }
        }
        byTex[t] = got;
        return got;
    };

    // Two-texture batches as the client builds their shader (0x00836980), Impostor.cpp's reading of the combiners.
    for (uint32_t bi = 0; bi < nBatch && !s_stop; ++bi) {
        const size_t b = batches + bi * 24;
        const uint32_t section = Rd16(s, b + 4), material = Rd16(s, b + 10), combo = Rd16(s, b + 16);
        if (section >= nSub) continue;
        const uint32_t blend = material < nMat ? Rd16(m, mats + material * 4 + 2) : 0;
        const uint32_t flags = material < nMat ? Rd16(m, mats + material * 4) : 0;
        // ponytail: blend 3 / 4 (additive glow) has no glow picture here (Impostor.cpp's Picture::glow); add one if
        // glowing trees (Crystalsong, Bloodmyst) read too dark far off.
        if (blend >= 3) continue;
        Layer l;
        const size_t sub = subs + section * 48;
        l.start = Rd16(s, sub + 8) + (uint32_t(Rd16(s, sub + 2)) << 16);
        l.count = Rd16(s, sub + 10);
        l.blend = blend;
        l.unlit = flags & 1;
        l.tex = texture(combo);
        BatchColour(m, s, b, l.colour);
        const uint16_t shaderId = Rd16(s, b + 2);
        if (Rd16(s, b + 14) >= 2 && nCombos && !(shaderId & 0x8000) && shaderId + 1u < nCombos && (l.tex2 = texture(combo + 1))) {
            uint16_t op0 = blend == 0 ? 0 : Rd16(m, combos + shaderId * 2), op = Rd16(m, combos + (shaderId + 1) * 2);
            if ((op0 == 3 && op != 1) || (op0 == 4 && op != 1 && op != 4)) op0 = op = 1;
            l.op2 = op == 3 || op == 7 ? kAdd : op == 4 || op == 6 ? kMul2 : kMul;
            l.alpha2 = op == 1 || op == 2 || op == 4 || op == 5 ? (op0 == 0 ? kAlphaT2 : kAlphaT1T2) : op == 3 ? kAlphaT1PlusT2 : kAlphaT1;
            l.alphaX2 = op == 4;
            const uint32_t tc = Rd16(s, b + 18) + 1u;
            l.uv2 = coords && tc < nCoord && Rd16(m, coords + tc * 2) == 1 ? 1 : 0;
        }
        M.layers.push_back(l);
    }
    return !M.layers.empty();
}

// Impostor.cpp: frame = max(zHi - zLo, 2 x reach) over every vertex, bottom = zLo.
bool Frame(const Mesh& M, ModelResult& r) {
    const std::vector<uint8_t>& m = M.m2;
    const uint32_t nVert = Rd32(m, 0x3C), ov = Rd32(m, 0x40);
    if (!nVert || uint64_t(ov) + uint64_t(nVert) * 48 > m.size()) return false;
    float zLo = 1e30f, zHi = -1e30f, reach = 0.0f;
    for (uint32_t v = 0; v < nVert; ++v) {
        const float x = RdF(m, ov + v * 48), y = RdF(m, ov + v * 48 + 4), z = RdF(m, ov + v * 48 + 8);
        zLo = std::min(zLo, z); zHi = std::max(zHi, z);
        reach = std::max(reach, std::sqrt(x * x + y * y));
    }
    r.frame = std::max(zHi - zLo, 2.0f * reach);
    r.bottom = zLo;
    r.height = zHi - zLo;
    r.reach = reach;
    return r.frame > 0.1f;
}

constexpr int C = 8;   // per texel: r g b coverage nx ny nz unlit

void Sample(const Image* t, float u, float v, uint8_t out[4]) {
    u -= std::floor(u); v -= std::floor(v);
    const uint8_t* q = &t->rgba[(size_t(std::min(uint32_t(v * t->h), t->h - 1)) * t->w + std::min(uint32_t(u * t->w), t->w - 1)) * 4];
    std::memcpy(out, q, 4);
}

// One view (angle view x 90 deg) at kPx, Impostor.cpp's raster, box-down, coverage and 8 fill passes: lv = kPx^2 x C.
bool RenderView(const Mesh& M, const ModelResult& r, uint32_t view, std::vector<float>& lv) {
    constexpr uint32_t kRaster = kPx * 2;
    struct Pixel { float depth = -1e30f; float rgb[3] = {}; float n[3] = {}; bool set = false, unlit = false; };
    const std::vector<uint8_t>& m = M.m2;
    const std::vector<uint8_t>& s = M.skin;
    const uint32_t nVert = Rd32(m, 0x3C), ov = Rd32(m, 0x40);
    const uint32_t nSkinVert = Rd32(s, 4), oSkinVert = Rd32(s, 8), nIdx = Rd32(s, 12), oIdx = Rd32(s, 16);
    if (uint64_t(oSkinVert) + uint64_t(nSkinVert) * 2 > s.size() || uint64_t(oIdx) + uint64_t(nIdx) * 2 > s.size()) return false;
    const float frame = r.frame, zLo = r.bottom, zHi = r.bottom + r.height;
    const float ang = float(view) * 1.5707963f, cx = std::cos(ang), cy = std::sin(ang);
    std::vector<Pixel> px(size_t(kRaster) * kRaster);
    bool any = false;
    for (const Layer& L : M.layers) {
        if (s_stop) return false;
        const Image* tex = L.tex;
        for (uint32_t i = 0; i + 2 < L.count && L.start + i + 2 < nIdx; i += 3) {
            float sx[3], sy[3], sd[3], uv[3][2], uv2[3][2], nr[3][3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const uint32_t si = Rd16(s, oIdx + (L.start + i + k) * 2);
                if (si >= nSkinVert) { ok = false; break; }
                const uint32_t vi = Rd16(s, oSkinVert + si * 2);
                if (vi >= nVert) { ok = false; break; }
                const size_t v = ov + size_t(vi) * 48;
                const float x = RdF(m, v), y = RdF(m, v + 4), z = RdF(m, v + 8);
                const float right = -cy * x + cx * y;
                sx[k] = (right / frame + 0.5f) * kRaster;
                sy[k] = (zHi - z) / frame * kRaster + (frame - (zHi - zLo)) / frame * kRaster;   // bottom-aligned
                sd[k] = cx * x + cy * y;
                uv[k][0] = RdF(m, v + 32); uv[k][1] = RdF(m, v + 36);
                uv2[k][0] = RdF(m, v + 32 + 8 * L.uv2); uv2[k][1] = RdF(m, v + 36 + 8 * L.uv2);
                const float nx = RdF(m, v + 20), ny = RdF(m, v + 24), nz = RdF(m, v + 28);
                nr[k][0] = -cy * nx + cx * ny; nr[k][1] = nz; nr[k][2] = cx * nx + cy * ny;
            }
            if (!ok) continue;
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, int(std::floor(std::min({ sx[0], sx[1], sx[2] }))));
            const int x1 = std::min(int(kRaster) - 1, int(std::ceil(std::max({ sx[0], sx[1], sx[2] }))));
            const int y0 = std::max(0, int(std::floor(std::min({ sy[0], sy[1], sy[2] }))));
            const int y1 = std::min(int(kRaster) - 1, int(std::ceil(std::max({ sy[0], sy[1], sy[2] }))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float qx = x + 0.5f, qy = y + 0.5f;
                    const float w0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
                    const float w1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float depth = w0 * sd[0] + w1 * sd[1] + w2 * sd[2];
                    Pixel& p = px[size_t(y) * kRaster + x];
                    if (depth <= p.depth) continue;
                    float rgb[3] = { 0.35f, 0.3f, 0.2f }, a = 1.0f;
                    if (tex) {
                        uint8_t t[4];
                        Sample(tex, w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0], w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1], t);
                        for (int j = 0; j < 3; ++j) rgb[j] = t[j] / 255.0f;
                        a = L.blend == 0 ? 1.0f : t[3] / 255.0f;
                    }
                    if (L.tex2) {
                        uint8_t q[4];
                        Sample(L.tex2, w0 * uv2[0][0] + w1 * uv2[1][0] + w2 * uv2[2][0], w0 * uv2[0][1] + w1 * uv2[1][1] + w2 * uv2[2][1], q);
                        if (L.op2 == kMul || L.op2 == kMul2)
                            for (int j = 0; j < 3; ++j) rgb[j] = std::min(1.0f, rgb[j] * q[j] / 255.0f * (L.op2 == kMul2 ? 2.0f : 1.0f));
                        if (L.blend != 0 && L.alpha2 == kAlphaT1PlusT2) a = std::min(1.0f, a + q[3] / 255.0f);
                        else if (L.blend != 0 && L.alpha2 != kAlphaT1)
                            a = std::min(1.0f, (L.alpha2 == kAlphaT2 ? 1.0f : a) * q[3] / 255.0f * (L.alphaX2 ? 2.0f : 1.0f));
                    }
                    // the game's keys: 224/255 for blend 1 (alpha-ref table 0x00AD8B7C), half for blended blend 2
                    if (a < (L.blend == 1 ? 224.0f / 255.0f : 0.5f)) continue;
                    float n[3];
                    for (int j = 0; j < 3; ++j) n[j] = w0 * nr[0][j] + w1 * nr[1][j] + w2 * nr[2][j];
                    if (n[2] < 0.0f) for (float& c : n) c = -c;   // two-sided: face the viewer
                    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    for (int j = 0; j < 3; ++j) {
                        p.n[j] = len > 0 ? n[j] / len : (j == 2 ? 1.0f : 0.0f);
                        p.rgb[j] = rgb[j] * L.colour[j];
                    }
                    p.depth = depth;
                    p.set = true;
                    p.unlit = L.unlit;
                    any = true;
                }
        }
    }
    lv.assign(size_t(kPx) * kPx * C, 0.0f);
    for (uint32_t y = 0; y < kPx; ++y)
        for (uint32_t x = 0; x < kPx; ++x) {
            float acc[7] = {};
            int n = 0;
            for (int k = 0; k < 4; ++k) {
                const Pixel& p = px[size_t(y * 2 + k / 2) * kRaster + x * 2 + k % 2];
                if (!p.set) continue;
                for (int j = 0; j < 3; ++j) { acc[j] += p.rgb[j]; acc[3 + j] += p.n[j]; }
                acc[6] += p.unlit ? 1.0f : 0.0f;
                ++n;
            }
            float* o = &lv[(size_t(y) * kPx + x) * C];
            o[3] = n / 4.0f;
            if (n) { for (int j = 0; j < 3; ++j) { o[j] = acc[j] / n; o[4 + j] = acc[3 + j] / n; } o[7] = acc[6] / n; }
        }
    // The empty texels take their drawn neighbours' colour, so filtering pulls no dark rim into the edge.
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<float> next = lv;
        for (uint32_t y = 0; y < kPx; ++y)
            for (uint32_t x = 0; x < kPx; ++x) {
                float* o = &next[(size_t(y) * kPx + x) * C];
                if (o[3] > 0.0f || o[4] != 0.0f || o[5] != 0.0f || o[6] != 0.0f) continue;
                float acc[7] = {};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = int(x) + dx, yy = int(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= int(kPx) || yy >= int(kPx)) continue;
                        const float* q = &lv[(size_t(yy) * kPx + xx) * C];
                        if (q[4] == 0.0f && q[5] == 0.0f && q[6] == 0.0f) continue;
                        for (int j = 0; j < 3; ++j) { acc[j] += q[j]; acc[3 + j] += q[4 + j]; }
                        acc[6] += q[7];
                        ++n;
                    }
                if (n) { for (int j = 0; j < 3; ++j) { o[j] = acc[j] / n; o[4 + j] = acc[3 + j] / n; } o[7] = acc[6] / n; }
            }
        lv.swap(next);
    }
    return any;
}

// A w x h, C-channel image halved (each side at least 1).
std::vector<float> Half(const std::vector<float>& img, uint32_t w, uint32_t h) {
    const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
    std::vector<float> out(size_t(hw) * hh * C, 0.0f);
    for (uint32_t y = 0; y < hh; ++y)
        for (uint32_t x = 0; x < hw; ++x) {
            float* o = &out[(size_t(y) * hw + x) * C];
            int n = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t xx = x * 2 + k % 2, yy = y * 2 + k / 2;
                if (xx >= w || yy >= h) continue;
                const float* q = &img[(size_t(yy) * w + xx) * C];
                for (int j = 0; j < C; ++j) o[j] += q[j];
                ++n;
            }
            for (int j = 0; j < C; ++j) o[j] /= float(n);
        }
    return out;
}

void BakeAtlas(const Mesh& M, ModelResult& r) {
    // Per view: kPx .. 1 px, each level box-filtered and its alpha scaled so it keeps level 0's coverage
    // (Impostor.cpp: a far tree does not thin out). Then side by side; levels past the last view-sized one
    // (2 x 1, 1 x 1) are plain boxes of the row, there for the device's chain only.
    std::vector<std::vector<float>> atlas(kChain);
    bool any = false;
    for (uint32_t view = 0; view < kViews && !s_stop; ++view) {
        std::vector<float> cur;
        any |= RenderView(M, r, view, cur);
        float coverage0 = 0.0f;
        for (size_t k = 0; k < size_t(kPx) * kPx; ++k) coverage0 += cur[k * C + 3] >= 0.5f;
        coverage0 /= float(kPx * kPx);
        uint32_t size = kPx;
        for (uint32_t l = 0; size >= 1 && l < kChain; ++l) {
            if (l) {
                cur = Half(cur, size, size);
                size /= 2;
                float lo = 0.5f, hi = 4.0f;
                for (int it = 0; it < 12; ++it) {
                    const float s = 0.5f * (lo + hi);
                    float c = 0.0f;
                    for (size_t k = 0; k < size_t(size) * size; ++k) c += cur[k * C + 3] * s >= 0.5f;
                    if (c / float(size * size) < coverage0) lo = s; else hi = s;
                }
                for (size_t k = 0; k < size_t(size) * size; ++k) cur[k * C + 3] = std::min(1.0f, cur[k * C + 3] * hi);
            }
            const uint32_t W = kViews * size;
            atlas[l].resize(size_t(W) * size * C);
            for (uint32_t y = 0; y < size; ++y)
                std::memcpy(&atlas[l][(size_t(y) * W + view * size) * C], &cur[size_t(y) * size * C], size_t(size) * C * 4);
            if (size == 1) break;
        }
    }
    if (s_stop || !any) return;
    uint32_t w = kAtlasW, h = kAtlasH;
    for (uint32_t l = 0; l < kChain; ++l, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u)) {
        if (atlas[l].empty()) atlas[l] = Half(atlas[l - 1], std::max(kAtlasW >> (l - 1), 1u), std::max(kAtlasH >> (l - 1), 1u));
        std::vector<float> alb(size_t(w) * h * 4), nrm(size_t(w) * h * 4);
        for (size_t k = 0; k < size_t(w) * h; ++k) {
            const float* q = &atlas[l][k * C];
            const float len = std::sqrt(q[4] * q[4] + q[5] * q[5] + q[6] * q[6]);
            for (int j = 0; j < 4; ++j) alb[k * 4 + j] = std::clamp(q[j], 0.0f, 1.0f);
            for (int j = 0; j < 2; ++j) nrm[k * 4 + j] = std::clamp((len > 0 ? q[4 + j] / len : 0.0f) * 0.5f + 0.5f, 0.0f, 1.0f);
            nrm[k * 4 + 2] = std::clamp(q[7], 0.0f, 1.0f);   // the unlit mask
            nrm[k * 4 + 3] = 1.0f;
        }
        r.albedo.push_back(EncodeDxt5(alb, w, h));
        r.normal.push_back(EncodeDxt5(nrm, w, h));
    }
}

// ---------------------------------------------------------------- cache: Cache/FarTrees/<fnv64 of the path>.ftc
//   'FTC1', u32 version, u64 fingerprint (M2 + skin + textures + version), f32 frame, bottom, height, reach,
//   f32 box[6], u32 levels (kChain); then albedo levels, then normal levels, each u32 size + DXT5 bytes.

uint32_t LevelBytes(uint32_t l) {
    return ((std::max(kAtlasW >> l, 1u) + 3) / 4) * ((std::max(kAtlasH >> l, 1u) + 3) / 4) * 16;
}

std::string CachePath(const std::string& model) {
    char name[64];
    std::snprintf(name, sizeof(name), "Cache/FarTrees/%016llx.ftc", static_cast<unsigned long long>(Fnv(model.data(), model.size())));
    return name;
}

bool ReadCache(const std::string& model, uint64_t fingerprint, ModelResult& r) {
    FILE* f = std::fopen(CachePath(model).c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    char magic[4];
    uint32_t version = 0, levels = 0;
    uint64_t fp = 0;
    if (std::fread(magic, 4, 1, f) == 1 && !std::memcmp(magic, "FTC1", 4) && std::fread(&version, 4, 1, f) == 1 && version == kCacheVersion
        && std::fread(&fp, 8, 1, f) == 1 && fp == fingerprint && std::fread(&r.frame, 4, 1, f) == 1 && std::fread(&r.bottom, 4, 1, f) == 1
        && std::fread(&r.height, 4, 1, f) == 1 && std::fread(&r.reach, 4, 1, f) == 1 && std::fread(r.box, 4, 6, f) == 6
        && std::fread(&levels, 4, 1, f) == 1 && levels == kChain) {
        ok = true;
        for (auto* set : { &r.albedo, &r.normal }) {
            if (!ok) break;
            set->resize(levels);
            for (uint32_t li = 0; li < levels; ++li) {
                auto& l = (*set)[li];
                uint32_t size = 0;
                if (std::fread(&size, 4, 1, f) != 1 || size != LevelBytes(li)) { ok = false; break; }
                l.resize(size);
                if (std::fread(l.data(), size, 1, f) != 1) { ok = false; break; }
            }
        }
    }
    std::fclose(f);
    return ok;
}

void WriteCache(const std::string& model, uint64_t fingerprint, const ModelResult& r) {
    std::error_code ec;
    std::filesystem::create_directories("Cache/FarTrees", ec);
    const std::string path = CachePath(model), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t levels = static_cast<uint32_t>(r.albedo.size());
    std::fwrite("FTC1", 4, 1, f);
    std::fwrite(&kCacheVersion, 4, 1, f);
    std::fwrite(&fingerprint, 8, 1, f);
    std::fwrite(&r.frame, 4, 1, f);
    std::fwrite(&r.bottom, 4, 1, f);
    std::fwrite(&r.height, 4, 1, f);
    std::fwrite(&r.reach, 4, 1, f);
    std::fwrite(r.box, 4, 6, f);
    std::fwrite(&levels, 4, 1, f);
    for (auto* set : { &r.albedo, &r.normal })
        for (auto& l : *set) {
            const uint32_t size = static_cast<uint32_t>(l.size());
            std::fwrite(&size, 4, 1, f);
            std::fwrite(l.data(), size, 1, f);
        }
    const bool ok = std::fclose(f) == 0;
    std::filesystem::remove(path, ec);
    if (ok) std::filesystem::rename(tmp, path, ec);
    else std::filesystem::remove(tmp, ec);
}

void BakeModel(const std::string& model, ModelResult& r) {
    r.model = model;
    Mesh m;
    if (!LoadMesh(model, m, false)) return;   // reads the files the fingerprint covers, decodes nothing
    if (ReadCache(model, m.fingerprint, r)) { r.ok = r.fromCache = true; return; }
    r.albedo.clear(); r.normal.clear();
    std::memcpy(r.box, m.box, sizeof(r.box));
    if (!Frame(m, r)) return;
    m = Mesh();
    if (s_stop || !LoadMesh(model, m, true)) return;
    BakeAtlas(m, r);
    if (s_stop || r.albedo.size() != kChain) return;
    r.ok = true;
    WriteCache(model, m.fingerprint, r);
}

void Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(s_lock);
            s_wake.wait(lock, [] { return s_stop || !s_jobs.empty(); });
            if (s_stop) return;
            job = std::move(s_jobs.front());
            s_jobs.pop_front();
        }
        // Out of memory (or anything else) fails this tile or model, never the client.
        if (job.tile) {
            TileResult r;
            try { ScanTile(job.a, job.x, job.y, r); } catch (...) { r.places.clear(); }
            std::lock_guard<std::mutex> lock(s_lock);
            s_tiles.push_back(std::move(r));
        } else {
            ModelResult r;
            try { BakeModel(job.a, r); } catch (...) { r = ModelResult(); r.model = job.a; }
            if (s_stop) return;
            std::lock_guard<std::mutex> lock(s_lock);
            s_models.push_back(std::move(r));
        }
    }
}

} // namespace

void Stop();
namespace {
// A joinable std::thread at static destruction would call std::terminate.
struct StopAtExit { ~StopAtExit() { Stop(); } } s_stopAtExit;
}

void Start() {
    if (s_thread.joinable()) return;
    s_stop = false;
    s_thread = std::thread(Run);
}

void Stop() {
    if (!s_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_stop = true;
        s_jobs.clear();
    }
    s_wake.notify_all();
    s_thread.join();
    std::lock_guard<std::mutex> lock(s_lock);
    s_tiles.clear();
    s_models.clear();
}

// Tiles go before models: placements first, then the pictures they need.
void RequestTile(const std::string& map, int32_t tx, int32_t ty) {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto it = std::find_if(s_jobs.begin(), s_jobs.end(), [](const Job& j) { return !j.tile; });
        s_jobs.insert(it, Job{ true, map, tx, ty });
    }
    s_wake.notify_one();
}

void CancelTile(const std::string& map, int32_t tx, int32_t ty) {
    std::lock_guard<std::mutex> lock(s_lock);
    s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(), [&](const Job& j) { return j.tile && j.x == tx && j.y == ty && j.a == map; }), s_jobs.end());
}

void RequestModel(const std::string& model) {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_jobs.push_back(Job{ false, model, 0, 0 });
    }
    s_wake.notify_one();
}

bool PopTile(TileResult& out) {
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_tiles.empty()) return false;
    out = std::move(s_tiles.front());
    s_tiles.pop_front();
    return true;
}

bool PopModel(ModelResult& out) {
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_models.empty()) return false;
    out = std::move(s_models.front());
    s_models.pop_front();
    return true;
}

} // namespace FarTreesBake
