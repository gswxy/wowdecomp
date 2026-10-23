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

#include "Engine/Source/BlizzardCore/SFile2-Core.h"

namespace FarTreesBake {

namespace {

constexpr float kMapC = 32.0f * 533.33333f;
constexpr uint32_t kCacheVersion = 1;   // bump when the picture code changes: every cache file re-bakes

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
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

std::string Lower(std::string s) {
    for (char& c : s) { if (c == '/') c = '\\'; else if (c >= 'A' && c <= 'Z') c = char(c + 32); }
    return s;
}

// A tree or bush by its file name (the leaf of the path), as Tools' wraithhost rules.
bool IsTree(const std::string& path) {
    const std::string leaf = path.substr(path.find_last_of('\\') + 1);
    auto has = [&](const char* w) { return leaf.find(w) != std::string::npos; };
    for (const char* w : { "street", "stump", "log0", "_log", "fallen", "treehouse", "leaves0", "fx_", "smoke", "firefl" })
        if (has(w)) return false;
    for (const char* w : { "tree", "pine", "spruce", "redwood", "palm", "oak", "willow", "birch", "aspen", "canopy",
                           "conifer", "cedar", "larch", "juniper", "cypress", "sapling", "bush", "shrub" })
        if (has(w)) return true;
    return false;
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
        std::string model = Lower(reinterpret_cast<const char*>(d.data() + mmdx + off));
        const size_t dot = model.rfind('.');
        if (dot != std::string::npos) model = model.substr(0, dot) + ".m2";
        if (!IsTree(model)) continue;
        Place p;
        p.model = model;
        p.uniqueId = Get<uint32_t>(d, e + 4);
        p.x = kMapC - Get<float>(d, e + 16);   // as CMap::CreateDoodadDef
        p.y = kMapC - Get<float>(d, e + 8);
        p.z = Get<float>(d, e + 12);
        float yaw = (Get<float>(d, e + 24) + 180.0f) / 360.0f;   // RotateAroundZ(rot.y + pi)
        p.yaw = yaw - std::floor(yaw);
        p.scale = Get<uint16_t>(d, e + 32) / 1024.0f;
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
    if (!off || off + size > d.size()) return false;
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
        if (n > size) return false;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* c = d.data() + 148 + 4 * p[i];
            uint8_t* o = &img.rgba[i * 4];
            o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
            if (alphaDepth == 8 && n + i < size) o[3] = p[n + i];
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

struct Batch { std::vector<uint32_t> idx; Image* tex; int blend; };
struct Mesh {
    std::vector<float> pos, nrm, uv;   // 3, 3, 2 per vertex
    std::vector<Batch> batches;
    std::vector<Image> textures;
    float box[6] = {};
    uint64_t fingerprint = 0;
};

bool LoadMesh(const std::string& path, Mesh& m) {
    std::vector<uint8_t> d, s;
    if (!ReadFile(path, d) || d.size() < 0x108 || std::memcmp(d.data(), "MD20", 4)) return false;
    if (!ReadFile(path.substr(0, path.size() - 3) + "00.skin", s) || s.size() < 48 || std::memcmp(s.data(), "SKIN", 4)) return false;
    m.fingerprint = Fnv(s.data(), s.size(), Fnv(d.data(), d.size(), Fnv(&kCacheVersion, 4)));
    const uint32_t nv = Get<uint32_t>(d, 0x3C), ov = Get<uint32_t>(d, 0x40);
    if (size_t(ov) + size_t(nv) * 48 > d.size()) return false;
    m.pos.resize(nv * 3); m.nrm.resize(nv * 3); m.uv.resize(nv * 2);
    for (uint32_t i = 0; i < nv; ++i) {
        std::memcpy(&m.pos[i * 3], &d[ov + i * 48], 12);
        std::memcpy(&m.nrm[i * 3], &d[ov + i * 48 + 20], 12);
        std::memcpy(&m.uv[i * 2], &d[ov + i * 48 + 32], 8);
    }
    for (int k = 0; k < 6; ++k) m.box[k] = Get<float>(d, 0xA0 + 4 * k);
    const uint32_t nt = Get<uint32_t>(d, 0x50), ot = Get<uint32_t>(d, 0x54);
    const uint32_t nm = Get<uint32_t>(d, 0x70), om = Get<uint32_t>(d, 0x74);
    const uint32_t nl = Get<uint32_t>(d, 0x80), ol = Get<uint32_t>(d, 0x84);
    m.textures.resize(nt);
    std::vector<int> texOk(nt, -1);
    const uint32_t ni = Get<uint32_t>(s, 4), oi = Get<uint32_t>(s, 8), nt3 = Get<uint32_t>(s, 12), ot3 = Get<uint32_t>(s, 16);
    const uint32_t nsub = Get<uint32_t>(s, 28), osub = Get<uint32_t>(s, 32), nu = Get<uint32_t>(s, 36), ou = Get<uint32_t>(s, 40);
    for (uint32_t i = 0; i < nu; ++i) {
        const size_t u = ou + 24 * size_t(i);
        const uint16_t sub = Get<uint16_t>(s, u + 4), mat = Get<uint16_t>(s, u + 10), combo = Get<uint16_t>(s, u + 16);
        if (sub >= nsub || mat >= nm || combo >= nl) continue;
        const int blend = Get<uint16_t>(d, om + 4 * mat + 2);
        const int16_t tl = Get<int16_t>(d, ol + 2 * combo);
        if (blend >= 3 || tl < 0 || uint32_t(tl) >= nt) continue;   // glow / additive: not in the picture
        if (texOk[tl] < 0) {
            texOk[tl] = 0;
            const size_t te = ot + 16 * size_t(tl);
            const uint32_t len = Get<uint32_t>(d, te + 8), off = Get<uint32_t>(d, te + 12);
            if (Get<uint32_t>(d, te) == 0 && len > 1 && size_t(off) + len <= d.size()) {
                std::vector<uint8_t> t;
                if (ReadFile(std::string(reinterpret_cast<const char*>(&d[off]), strnlen(reinterpret_cast<const char*>(&d[off]), len)), t)) {
                    m.fingerprint = Fnv(t.data(), t.size(), m.fingerprint);
                    texOk[tl] = DecodeBlp(t, m.textures[tl]) ? 1 : 0;
                }
            }
        }
        if (texOk[tl] != 1) continue;   // replaceable or unreadable texture
        const size_t sm = osub + 48 * size_t(sub);
        const uint32_t start = Get<uint16_t>(s, sm + 8) + (uint32_t(Get<uint16_t>(s, sm + 2)) << 16), count = Get<uint16_t>(s, sm + 10);
        Batch b;
        b.tex = &m.textures[tl];
        b.blend = blend;
        for (uint32_t k = start; k < start + count && k < nt3; ++k) {
            const uint16_t li = Get<uint16_t>(s, ot3 + 2 * size_t(k));
            if (li >= ni) { b.idx.clear(); break; }
            const uint16_t vi = Get<uint16_t>(s, oi + 2 * size_t(li));
            if (vi >= nv) { b.idx.clear(); break; }
            b.idx.push_back(vi);
        }
        b.idx.resize(b.idx.size() / 3 * 3);
        if (!b.idx.empty()) m.batches.push_back(std::move(b));
    }
    return !m.batches.empty();
}

// ---------------------------------------------------------------- views

void Decode(uint32_t i, uint32_t j, float d[3]) {   // grid point -> view direction (FarTreesVS Decode)
    const float px = float(i) / (kFrames - 1) * 2 - 1, py = float(j) / (kFrames - 1) * 2 - 1;
    const float hx = (px + py) * 0.5f, hy = (px - py) * 0.5f;
    d[0] = hx; d[1] = hy; d[2] = 1 - std::fabs(hx) - std::fabs(hy);
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) d[k] /= l;
}

void Basis(const float d[3], float r[3], float u[3]) {   // right = cross(z, d), up = cross(d, right)
    r[0] = -d[1] + 1e-5f; r[1] = d[0]; r[2] = 0;
    const float l = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    r[0] /= l; r[1] /= l;
    u[0] = d[1] * r[2] - d[2] * r[1]; u[1] = d[2] * r[0] - d[0] * r[2]; u[2] = d[0] * r[1] - d[1] * r[0];
}

// One view at n x n samples into col (rgb + coverage) and nd (normal + depth), n x n x 4 each.
void Raster(const Mesh& m, const float c[3], float R, const float d[3], int n, std::vector<float>& col, std::vector<float>& nd) {
    float r[3], u[3];
    Basis(d, r, u);
    const size_t nv = m.pos.size() / 3;
    std::vector<float> sx(nv), sy(nv), sz(nv);
    for (size_t i = 0; i < nv; ++i) {
        const float q[3] = { (m.pos[i * 3] - c[0]) / R, (m.pos[i * 3 + 1] - c[1]) / R, (m.pos[i * 3 + 2] - c[2]) / R };
        sx[i] = ((q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) * 0.5f + 0.5f) * n;
        sy[i] = (0.5f - (q[0] * u[0] + q[1] * u[1] + q[2] * u[2]) * 0.5f) * n;
        sz[i] = q[0] * d[0] + q[1] * d[1] + q[2] * d[2];
    }
    std::vector<float> zb(size_t(n) * n, -1e9f);
    col.assign(size_t(n) * n * 4, 0.0f);
    nd.assign(size_t(n) * n * 4, 0.0f);
    for (const Batch& b : m.batches) {
        const Image& t = *b.tex;
        for (size_t k = 0; k < b.idx.size(); k += 3) {
            const uint32_t a = b.idx[k], bb = b.idx[k + 1], cc = b.idx[k + 2];
            const float x0 = sx[a], y0 = sy[a], x1 = sx[bb], y1 = sy[bb], x2 = sx[cc], y2 = sy[cc];
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(area) < 1e-9f) continue;
            const int ix0 = std::max(int(std::min({ x0, x1, x2 })), 0), ix1 = std::min(int(std::max({ x0, x1, x2 })) + 1, n);
            const int iy0 = std::max(int(std::min({ y0, y1, y2 })), 0), iy1 = std::min(int(std::max({ y0, y1, y2 })) + 1, n);
            for (int y = iy0; y < iy1; ++y)
                for (int x = ix0; x < ix1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / area;
                    const float w2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / area;
                    const float w0 = 1 - w1 - w2;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float z = w0 * sz[a] + w1 * sz[bb] + w2 * sz[cc];
                    float& zz = zb[size_t(y) * n + x];
                    if (z <= zz) continue;
                    const float tu = w0 * m.uv[a * 2] + w1 * m.uv[bb * 2] + w2 * m.uv[cc * 2];
                    const float tv = w0 * m.uv[a * 2 + 1] + w1 * m.uv[bb * 2 + 1] + w2 * m.uv[cc * 2 + 1];
                    const int tx = ((int(std::floor(tu * t.w)) % int(t.w)) + int(t.w)) % int(t.w);
                    const int ty = ((int(std::floor(tv * t.h)) % int(t.h)) + int(t.h)) % int(t.h);
                    const uint8_t* tp = &t.rgba[(size_t(ty) * t.w + tx) * 4];
                    if ((b.blend == 1 && tp[3] < 224) || (b.blend == 2 && tp[3] < 128)) continue;   // the game's keys
                    zz = z;
                    float nn[3];
                    for (int q = 0; q < 3; ++q) nn[q] = w0 * m.nrm[a * 3 + q] + w1 * m.nrm[bb * 3 + q] + w2 * m.nrm[cc * 3 + q];
                    float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-9f;
                    if (nn[0] * d[0] + nn[1] * d[1] + nn[2] * d[2] < 0) l = -l;   // two-sided: face the view
                    float* o = &col[(size_t(y) * n + x) * 4];
                    o[0] = tp[0] / 255.0f; o[1] = tp[1] / 255.0f; o[2] = tp[2] / 255.0f; o[3] = 1;
                    float* p = &nd[(size_t(y) * n + x) * 4];
                    p[0] = nn[0] / l; p[1] = nn[1] / l; p[2] = nn[2] / l; p[3] = z;
                }
        }
    }
}

// Fill the colour of empty texels (alpha 0) from covered neighbours: no dark rims under bilinear and mips.
void Dilate(std::vector<float>& img, const std::vector<float>& cover, uint32_t w, int iters) {
    std::vector<uint8_t> has(cover.size());
    for (size_t i = 0; i < cover.size(); ++i) has[i] = cover[i] > 0;
    for (int it = 0; it < iters; ++it) {
        std::vector<uint8_t> next = has;
        bool any = false;
        for (uint32_t y = 0; y < w; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (has[i]) continue;
                float acc[3] = {}; int cnt = 0;
                const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (auto& o : nb) {
                    const int xx = int(x) + o[0], yy = int(y) + o[1];
                    if (xx < 0 || yy < 0 || xx >= int(w) || yy >= int(w)) continue;
                    const size_t j = size_t(yy) * w + xx;
                    if (!has[j]) continue;
                    for (int k = 0; k < 3; ++k) acc[k] += img[j * 4 + k];
                    ++cnt;
                }
                if (!cnt) continue;
                for (int k = 0; k < 3; ++k) img[i * 4 + k] = acc[k] / cnt;
                next[i] = 1; any = true;
            }
        has.swap(next);
        if (!any) break;
    }
}

// Mip chain as DXT5. coverKeep: alpha of each level rescaled so the 0.5 test keeps level 0's coverage.
std::vector<std::vector<uint8_t>> Mips(std::vector<float> img, uint32_t w, bool coverKeep) {
    std::vector<std::vector<uint8_t>> out;
    size_t covered0 = 0;
    for (size_t i = 0; i < size_t(w) * w; ++i) covered0 += img[i * 4 + 3] >= 0.5f;
    const float base = float(covered0) / float(size_t(w) * w);
    for (;;) {
        out.push_back(EncodeDxt5(img, w, w));
        if (w == 1) break;
        const uint32_t h = w / 2;
        std::vector<float> m(size_t(h) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < h; ++x)
                for (int k = 0; k < 4; ++k)
                    m[(size_t(y) * h + x) * 4 + k] = 0.25f * (img[((size_t(2 * y) * w) + 2 * x) * 4 + k] + img[((size_t(2 * y) * w) + 2 * x + 1) * 4 + k]
                        + img[((size_t(2 * y + 1) * w) + 2 * x) * 4 + k] + img[((size_t(2 * y + 1) * w) + 2 * x + 1) * 4 + k]);
        if (coverKeep && base > 0 && base < 1) {
            std::vector<float> a(size_t(h) * h);
            for (size_t i = 0; i < a.size(); ++i) a[i] = m[i * 4 + 3];
            const size_t at = std::min(a.size() - 1, size_t((1 - base) * a.size()));
            std::nth_element(a.begin(), a.begin() + at, a.end());
            const float t = a[at];
            if (t > 1e-3f) for (size_t i = 0; i < a.size(); ++i) m[i * 4 + 3] = std::min(m[i * 4 + 3] * (0.5f / t), 1.0f);
        }
        img.swap(m);
        w = h;
    }
    return out;
}

void BakeAtlas(const Mesh& m, ModelResult& r) {
    // Centre and radius of the drawn vertices.
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const Batch& b : m.batches) for (uint32_t i : b.idx) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], m.pos[i * 3 + k]); hi[k] = std::max(hi[k], m.pos[i * 3 + k]); }
    for (int k = 0; k < 3; ++k) r.centre[k] = (lo[k] + hi[k]) * 0.5f;
    float R = 0;
    for (const Batch& b : m.batches) for (uint32_t i : b.idx) {
        float s = 0;
        for (int k = 0; k < 3; ++k) { const float t = m.pos[i * 3 + k] - r.centre[k]; s += t * t; }
        R = std::max(R, s);
    }
    r.radius = std::sqrt(R) * 1.02f + 1e-3f;
    r.extent = std::max({ m.box[3] - m.box[0], m.box[4] - m.box[1], m.box[5] - m.box[2] });
    r.framePx = r.extent >= 40.0f ? 128 : 64;
    const uint32_t px = r.framePx, ss = px <= 64 ? 4 : 2, n = px * ss, W = kFrames * px;
    std::vector<float> alb(size_t(W) * W * 4, 0.0f), nrm(size_t(W) * W * 4, 0.0f), cover(size_t(W) * W, 0.0f);
    std::vector<float> col, nd;
    for (uint32_t i = 0; i < kFrames && !s_stop; ++i)
        for (uint32_t j = 0; j < kFrames; ++j) {
            float d[3];
            Decode(i, j, d);
            Raster(m, r.centre, r.radius, d, int(n), col, nd);
            for (uint32_t y = 0; y < px; ++y)
                for (uint32_t x = 0; x < px; ++x) {
                    float c = 0, rgb[3] = {}, nn[3] = {}, z = 0;
                    for (uint32_t sy = 0; sy < ss; ++sy)
                        for (uint32_t sx = 0; sx < ss; ++sx) {
                            const size_t s = (size_t(y * ss + sy) * n + x * ss + sx) * 4;
                            if (col[s + 3] <= 0) continue;
                            c += 1;
                            for (int k = 0; k < 3; ++k) { rgb[k] += col[s + k]; nn[k] += nd[s + k]; }
                            z += nd[s + 3];
                        }
                    const size_t o = (size_t(j * px + y) * W + i * px + x);
                    float* a = &alb[o * 4];
                    float* q = &nrm[o * 4];
                    if (c > 0) {
                        const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-9f;
                        for (int k = 0; k < 3; ++k) { a[k] = rgb[k] / c; q[k] = nn[k] / l * 0.5f + 0.5f; }
                        q[3] = z / c * 0.5f + 0.5f;
                    } else {
                        q[3] = 0.5f;
                    }
                    a[3] = c / float(ss * ss);
                    cover[o] = a[3];
                }
        }
    Dilate(alb, cover, W, 16);
    Dilate(nrm, cover, W, 16);
    r.albedo = Mips(std::move(alb), W, true);
    r.normal = Mips(std::move(nrm), W, false);
}

// ---------------------------------------------------------------- cache: Cache/FarTrees/<fnv64 of the path>.ftc
//   'FTC1', u32 version, u64 fingerprint (M2 + skin + textures + version), f32 centre[3], radius, extent,
//   u32 framePx, u32 levels; then albedo levels, then normal levels, each u32 size + DXT5 bytes.

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
        && std::fread(&fp, 8, 1, f) == 1 && fp == fingerprint && std::fread(r.centre, 4, 3, f) == 3 && std::fread(&r.radius, 4, 1, f) == 1
        && std::fread(&r.extent, 4, 1, f) == 1 && std::fread(&r.framePx, 4, 1, f) == 1 && std::fread(&levels, 4, 1, f) == 1 && levels <= 16
        && (r.framePx == 64 || r.framePx == 128)) {
        ok = true;
        for (auto* set : { &r.albedo, &r.normal }) {
            set->resize(levels);
            for (auto& l : *set) {
                uint32_t size = 0;
                if (std::fread(&size, 4, 1, f) != 1 || size > (64u << 20)) { ok = false; break; }
                l.resize(size);
                if (size && std::fread(l.data(), size, 1, f) != 1) { ok = false; break; }
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
    std::fwrite(r.centre, 4, 3, f);
    std::fwrite(&r.radius, 4, 1, f);
    std::fwrite(&r.extent, 4, 1, f);
    std::fwrite(&r.framePx, 4, 1, f);
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
    if (!LoadMesh(model, m)) return;   // also reads the files the fingerprint covers
    if (ReadCache(model, m.fingerprint, r)) { r.ok = r.fromCache = true; return; }
    r.albedo.clear(); r.normal.clear();
    BakeAtlas(m, r);
    if (s_stop) return;
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
        if (job.tile) {
            TileResult r;
            ScanTile(job.a, job.x, job.y, r);
            std::lock_guard<std::mutex> lock(s_lock);
            s_tiles.push_back(std::move(r));
        } else {
            ModelResult r;
            BakeModel(job.a, r);
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
