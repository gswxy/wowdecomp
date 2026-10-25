#include "WoW/Source/WorldClient/FarLandBake.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace FarLandBake {
namespace {

constexpr float kTile = 1600.0f / 3.0f;
constexpr float kLiquid[4][3] = { { 0.10f, 0.17f, 0.20f }, { 0.07f, 0.13f, 0.18f }, { 0.85f, 0.30f, 0.05f }, { 0.25f, 0.45f, 0.10f } };

struct Span {
    const uint8_t* p;
    size_t n;
    bool Has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    template <class T> T Get(size_t off) const { T v; std::memcpy(&v, p + off, sizeof(T)); return v; }
};

// Top-level chunks: tag as written in the file (reversed: "KNCM" is MCNK).
struct Chunk { size_t data; uint32_t size; };
void Walk(const Span& s, size_t from, size_t to, std::vector<std::pair<uint32_t, Chunk>>& out) {
    for (size_t i = from; i + 8 <= to;) {
        const uint32_t tag = s.Get<uint32_t>(i), size = s.Get<uint32_t>(i + 4);
        if (!s.Has(i + 8, size)) break;
        out.push_back({ tag, { i + 8, size } });
        i += 8 + size;
    }
}
constexpr uint32_t Tag(const char (&t)[5]) {   // "MCNK" -> the little-endian u32 the file holds
    return uint32_t(uint8_t(t[3])) | uint32_t(uint8_t(t[2])) << 8 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[0])) << 24;
}

void Rgb565(uint16_t c, float out[3]) {
    out[0] = ((c >> 11) & 31) * (255.0f / 31.0f);
    out[1] = ((c >> 5) & 63) * (255.0f / 63.0f);
    out[2] = (c & 31) * (255.0f / 31.0f);
}

// Mean rgb (0..255) of a BLP2's 4 x 4 mip (or its smallest). Grey when unreadable.
uint32_t TexMean(Context& ctx, const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    auto it = ctx.texMean.find(key);
    if (it != ctx.texMean.end()) return it->second;
    float sum[3] = { 128.0f, 128.0f, 128.0f };
    std::vector<uint8_t> d;
    if (ctx.read(name, d) && d.size() >= 1172 && std::memcmp(d.data(), "BLP2", 4) == 0) {
        Span s { d.data(), d.size() };
        const uint8_t enc = d[8], aenc = d[10];
        const uint32_t w = s.Get<uint32_t>(12), h = s.Get<uint32_t>(16);
        uint32_t lv = 0;
        while (lv + 1 < 16 && s.Get<uint32_t>(84 + 4 * (lv + 1)) && std::max(w >> (lv + 1), 1u) >= 4 && std::max(h >> (lv + 1), 1u) >= 4) lv++;
        const uint32_t o = s.Get<uint32_t>(20 + 4 * lv), mw = std::max(w >> lv, 1u), mh = std::max(h >> lv, 1u);
        float acc[3] = {};
        uint32_t cnt = 0;
        if (enc == 2) {
            const uint32_t step = aenc == 0 ? 8 : 16, blocks = std::max(mw / 4, 1u) * std::max(mh / 4, 1u);
            for (uint32_t b = 0; b < blocks && s.Has(o + b * step, step); b++) {
                const size_t c = o + b * step + step - 8;
                const uint16_t c0 = s.Get<uint16_t>(c), c1 = s.Get<uint16_t>(c + 2);
                const uint32_t bits = s.Get<uint32_t>(c + 4);
                float p[4][3];
                Rgb565(c0, p[0]);
                Rgb565(c1, p[1]);
                for (int j = 0; j < 3; j++) {
                    p[2][j] = c0 > c1 ? (2 * p[0][j] + p[1][j]) / 3 : (p[0][j] + p[1][j]) / 2;
                    p[3][j] = c0 > c1 ? (p[0][j] + 2 * p[1][j]) / 3 : 0.0f;
                }
                for (int k = 0; k < 16; k++, cnt++)
                    for (int j = 0; j < 3; j++) acc[j] += p[(bits >> (2 * k)) & 3][j];
            }
        } else if (enc == 1 && s.Has(o, size_t(mw) * mh)) {
            for (uint32_t k = 0; k < mw * mh; k++, cnt++)
                for (int j = 0; j < 3; j++) acc[j] += d[148 + d[o + k] * 4 + 2 - j];
        } else if (enc == 3 && s.Has(o, size_t(mw) * mh * 4)) {
            for (uint32_t k = 0; k < mw * mh; k++, cnt++)
                for (int j = 0; j < 3; j++) acc[j] += d[o + k * 4 + 2 - j];
        }
        if (cnt)
            for (int j = 0; j < 3; j++) sum[j] = acc[j] / cnt;
    }
    const uint32_t v = uint32_t(sum[0] + 0.5f) << 16 | uint32_t(sum[1] + 0.5f) << 8 | uint32_t(sum[2] + 0.5f);
    ctx.texMean[key] = v;
    return v;
}

// ---------- DXT encoders (range fit along the block's main colour axis) ----------
uint16_t To565(const float c[3]) {
    auto q = [](float v, float m) { return uint32_t(std::min(std::max(std::lround(v * m / 255.0f), 0L), long(m))); };
    return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

void Dxt1(const float px[16][3], uint8_t out[8]) {
    float mean[3] = {};
    for (int k = 0; k < 16; k++)
        for (int j = 0; j < 3; j++) mean[j] += px[k][j] / 16;
    float cov[3][3] = {};
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) cov[i][j] += (px[k][i] - mean[i]) * (px[k][j] - mean[j]);
    float ax[3] = { 1, 1, 1 };
    for (int it = 0; it < 4; it++) {   // power iteration
        float n[3] = {};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) n[i] += cov[i][j] * ax[j];
        const float m = std::max(std::max(std::fabs(n[0]), std::fabs(n[1])), std::max(std::fabs(n[2]), 1e-9f));
        for (int i = 0; i < 3; i++) ax[i] = n[i] / m;
    }
    int lo = 0, hi = 0;
    float pmin = 1e30f, pmax = -1e30f;
    for (int k = 0; k < 16; k++) {
        const float p = (px[k][0] - mean[0]) * ax[0] + (px[k][1] - mean[1]) * ax[1] + (px[k][2] - mean[2]) * ax[2];
        if (p < pmin) { pmin = p; lo = k; }
        if (p > pmax) { pmax = p; hi = k; }
    }
    uint16_t c0 = To565(px[hi]), c1 = To565(px[lo]);
    if (c0 < c1) std::swap(c0, c1);
    float pal[4][3];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int j = 0; j < 3; j++) {
        pal[2][j] = (2 * pal[0][j] + pal[1][j]) / 3;
        pal[3][j] = (pal[0][j] + 2 * pal[1][j]) / 3;
    }
    uint32_t bits = 0;
    if (c0 != c1)
        for (int k = 0; k < 16; k++) {
            int best = 0;
            float bd = 1e30f;
            for (int c = 0; c < 4; c++) {
                const float d = (px[k][0] - pal[c][0]) * (px[k][0] - pal[c][0]) + (px[k][1] - pal[c][1]) * (px[k][1] - pal[c][1]) + (px[k][2] - pal[c][2]) * (px[k][2] - pal[c][2]);
                if (d < bd) { bd = d; best = c; }
            }
            bits |= uint32_t(best) << (2 * k);
        }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &bits, 4);
}

void Dxt5Alpha(const float a[16], uint8_t out[8]) {
    float mx = 0, mn = 255;
    for (int k = 0; k < 16; k++) { mx = std::max(mx, a[k]); mn = std::min(mn, a[k]); }
    const int a0 = int(std::lround(std::min(std::max(mx, 0.0f), 255.0f))), a1 = int(std::lround(std::min(std::max(mn, 0.0f), 255.0f)));
    uint64_t bits = 0;
    if (a0 > a1)
        for (int k = 0; k < 16; k++) {
            const int t = int(std::lround((a[k] - a1) * 7.0f / float(a0 - a1)));
            const int q = std::min(std::max(t, 0), 7);
            const uint64_t code = q == 7 ? 0 : q == 0 ? 1 : uint64_t(8 - q);
            bits |= code << (3 * k);
        }
    out[0] = uint8_t(a0);
    out[1] = uint8_t(a1);
    for (int i = 0; i < 6; i++) out[2 + i] = uint8_t(bits >> (8 * i));
}

// img (px x px x 4, rgba 0..255) -> its levels down to 4 x 4, DXT1 (rgb) or DXT5 (rgba), into out.
void Encode(std::vector<float> img, uint32_t px, uint32_t levels, bool dxt5, uint8_t* out) {
    for (uint32_t l = 0; l < levels; l++) {
        const uint32_t nb = px / 4;
        for (uint32_t by = 0; by < nb; by++)
            for (uint32_t bx = 0; bx < nb; bx++) {
                float c[16][3], a[16];
                for (int k = 0; k < 16; k++) {
                    const float* p = &img[((by * 4 + k / 4) * px + bx * 4 + k % 4) * 4];
                    c[k][0] = p[0]; c[k][1] = p[1]; c[k][2] = p[2]; a[k] = p[3];
                }
                if (dxt5) { Dxt5Alpha(a, out); out += 8; }
                Dxt1(c, out);
                out += 8;
            }
        if (l + 1 == levels) break;
        const uint32_t h = px / 2;
        std::vector<float> next(size_t(h) * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < h; x++)
                for (int j = 0; j < 4; j++)
                    next[(y * h + x) * 4 + j] = 0.25f * (img[((2 * y) * px + 2 * x) * 4 + j] + img[((2 * y) * px + 2 * x + 1) * 4 + j]
                                                       + img[((2 * y + 1) * px + 2 * x) * 4 + j] + img[((2 * y + 1) * px + 2 * x + 1) * 4 + j]);
        img.swap(next);
        px = h;
    }
}

} // namespace

uint32_t Crc32(const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static const bool init = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

size_t AlbedoLevelOffset(uint32_t level) {
    size_t o = 0;
    for (uint32_t l = 0; l < level; l++) o += size_t((kAlbedoPx >> l) / 4) * ((kAlbedoPx >> l) / 4) * 8;
    return o;
}

size_t NormalLevelOffset(uint32_t level) {
    size_t o = 0;
    for (uint32_t l = 0; l < level; l++) o += size_t((kNormalPx >> l) / 4) * ((kNormalPx >> l) / 4) * 16;
    return o;
}

void SolidDxt1(const uint8_t rgb[3], uint8_t out[8]) {
    const float c[3] = { float(rgb[0]), float(rgb[1]), float(rgb[2]) };
    const uint16_t v = To565(c);
    std::memcpy(out, &v, 2);
    std::memcpy(out + 2, &v, 2);
    std::memset(out + 4, 0, 4);
}

uint32_t TexturesKey(Context& ctx, const uint8_t* adt, size_t size) {
    const Span s { adt, size };
    std::vector<std::pair<uint32_t, Chunk>> top;
    Walk(s, 0, s.n, top);
    std::vector<uint32_t> means;
    for (auto& c : top)
        if (c.first == Tag("MTEX")) {
            const char* p = reinterpret_cast<const char*>(adt + c.second.data);
            for (size_t i = 0; i < c.second.size;) {
                const size_t len = strnlen(p + i, c.second.size - i);
                if (len) means.push_back(TexMean(ctx, std::string(p + i, len)));
                i += len + 1;
            }
        }
    return Crc32(reinterpret_cast<const uint8_t*>(means.data()), means.size() * 4);
}

bool ReadBigAlpha(const std::vector<uint8_t>& wdt) {
    Span s { wdt.data(), wdt.size() };
    std::vector<std::pair<uint32_t, Chunk>> top;
    Walk(s, 0, s.n, top);
    for (auto& c : top)
        if (c.first == Tag("MPHD") && c.second.size >= 4) return (s.Get<uint32_t>(c.second.data) & 0x4) != 0;
    return false;
}

bool Bake(Context& ctx, const uint8_t* adt, size_t size, Tile& out) {
    const Span s { adt, size };
    std::vector<std::pair<uint32_t, Chunk>> top;
    Walk(s, 0, s.n, top);
    std::vector<uint32_t> means;   // per MTEX entry, 0xRRGGBB
    size_t mh2o = 0;
    bool haveMh2o = false;
    for (auto& c : top) {
        if (c.first == Tag("MTEX")) {
            const char* p = reinterpret_cast<const char*>(adt + c.second.data);
            for (size_t i = 0; i < c.second.size;) {
                const size_t len = strnlen(p + i, c.second.size - i);
                if (len) means.push_back(TexMean(ctx, std::string(p + i, len)));
                i += len + 1;
            }
        } else if (c.first == Tag("MH2O")) {
            mh2o = c.second.data;
            haveMh2o = true;
        }
    }
    auto layerRgb = [&](uint32_t tex, float rgb[3]) {
        const uint32_t v = means.empty() ? 0x808080u : means[std::min<size_t>(tex, means.size() - 1)];
        rgb[0] = float(v >> 16 & 255) / 255.0f; rgb[1] = float(v >> 8 & 255) / 255.0f; rgb[2] = float(v & 255) / 255.0f;
    };

    std::vector<float> col(size_t(kAlbedoPx) * kAlbedoPx * 3, 0.5f);
    std::vector<float> hgt(129 * 129, 0.0f);
    std::vector<int8_t> liq(size_t(kAlbedoPx) * kAlbedoPx, -1);
    uint32_t chunks = 0;
    for (auto& top1 : top) {
        if (top1.first != Tag("MCNK") || top1.second.size < 128) continue;
        const size_t p = top1.second.data, c0 = p - 8;
        const uint32_t flags = s.Get<uint32_t>(p), ix = s.Get<uint32_t>(p + 4), iy = s.Get<uint32_t>(p + 8), nl = s.Get<uint32_t>(p + 12);
        if (ix > 15 || iy > 15) continue;
        const uint32_t oH = s.Get<uint32_t>(p + 0x14), oLy = s.Get<uint32_t>(p + 0x1C), oAl = s.Get<uint32_t>(p + 0x24), szAl = s.Get<uint32_t>(p + 0x28);
        const float z0 = s.Get<float>(p + 0x70);
        const uint32_t oCv = s.Get<uint32_t>(p + 0x74);
        if (!s.Has(c0 + oH + 8, 145 * 4)) continue;
        chunks++;
        for (int r = 0; r < 9; r++)
            for (int q = 0; q < 9; q++) hgt[(iy * 8 + r) * 129 + ix * 8 + q] = s.Get<float>(c0 + oH + 8 + (r * 17 + q) * 4) + z0;

        // audit 8: no MH2O, so the chunk's MCLQ (its first liquid: MCNK flags 0x4 river .. 0x20 slime = LiquidType 1..4,
        // as CMapChunk does), a cell wet unless its tile is 0x0F (no liquid); the whole chunk when MCLQ is unreadable.
        const uint32_t oLq = s.Get<uint32_t>(p + 0x60), szLq = s.Get<uint32_t>(p + 0x64);
        if (!haveMh2o && (flags & 0x3C)) {
            uint32_t type = 1;
            while (!(flags & (4u << (type - 1)))) type++;
            const int8_t cat = int8_t(type < ctx.liquidCategory.size() ? std::min<uint8_t>(ctx.liquidCategory[type], 3) : 0);
            const bool tiles = szLq > 8 && s.Has(c0 + oLq + 8 + 0x290, 64);
            for (int r = 0; r < 8; r++)
                for (int q = 0; q < 8; q++)
                    if (!tiles || (adt[c0 + oLq + 8 + 0x290 + r * 8 + q] & 0x0F) != 0x0F) liq[(iy * 8 + r) * kAlbedoPx + ix * 8 + q] = cat;
        }

        float c[8][8][3];
        uint32_t tex[4] = {};
        uint32_t lflags[4] = {}, lofs[4] = {};
        uint32_t n = std::min(nl, 4u);
        for (uint32_t k = 0; k < n; k++) {
            if (!s.Has(c0 + oLy + 8 + 16 * k, 16)) {
                n = k;   // audit 10: a truncated layer table keeps the layers before it, not fail the tile
                break;
            }
            tex[k] = s.Get<uint32_t>(c0 + oLy + 8 + 16 * k);
            lflags[k] = s.Get<uint32_t>(c0 + oLy + 8 + 16 * k + 4);
            lofs[k] = s.Get<uint32_t>(c0 + oLy + 8 + 16 * k + 8);
        }
        float base[3] = { 0.5f, 0.5f, 0.5f };
        if (n) layerRgb(tex[0], base);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                for (int j = 0; j < 3; j++) c[y][x][j] = base[j];
        float rest[8][8] = {};   // big alpha: 1 - sum of the upper layers
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) rest[y][x] = 1.0f;
        float upper[3][8][8][3] = {};
        for (uint32_t k = 1; k < n && szAl > 8; k++) {
            if (!(lflags[k] & 0x100)) continue;   // audit 9: no alpha map (MCLY 0x100), as stock builds none for it
            uint8_t a[4096];
            size_t q = c0 + oAl + 8 + lofs[k];
            if (ctx.bigAlpha && (lflags[k] & 0x200)) {
                size_t got = 0;
                while (got < 4096 && s.Has(q, 1)) {
                    const uint8_t b = adt[q++], cnt = b & 0x7F;
                    if (b & 0x80) {
                        if (!s.Has(q, 1)) break;
                        for (uint32_t i = 0; i < cnt && got < 4096; i++) a[got++] = adt[q];
                        q++;
                    } else {
                        for (uint32_t i = 0; i < cnt && got < 4096 && s.Has(q, 1); i++) a[got++] = adt[q++];
                    }
                }
                std::memset(a + got, 0, 4096 - got);
            } else if (ctx.bigAlpha) {
                if (!s.Has(q, 4096)) break;
                std::memcpy(a, adt + q, 4096);
            } else {
                if (!s.Has(q, 2048)) break;
                for (int i = 0; i < 2048; i++) {
                    a[2 * i] = uint8_t((adt[q + i] & 15) * 17);
                    a[2 * i + 1] = uint8_t((adt[q + i] >> 4) * 17);
                }
                if (!(flags & 0x8000)) {   // not "do not fix alpha map": the last row and column repeat their neighbours
                    for (int i = 0; i < 64; i++) { a[i * 64 + 63] = a[i * 64 + 62]; a[63 * 64 + i] = a[62 * 64 + i]; }
                }
            }
            float lrgb[3];
            layerRgb(tex[k], lrgb);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    float m = 0;
                    for (int yy = 0; yy < 8; yy++)
                        for (int xx = 0; xx < 8; xx++) m += a[(y * 8 + yy) * 64 + x * 8 + xx];
                    m /= 64.0f * 255.0f;
                    if (ctx.bigAlpha) {
                        rest[y][x] -= m;
                        for (int j = 0; j < 3; j++) upper[k - 1][y][x][j] = lrgb[j] * m;
                    } else {
                        for (int j = 0; j < 3; j++) c[y][x][j] += (lrgb[j] - c[y][x][j]) * m;
                    }
                }
        }
        if (ctx.bigAlpha)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    for (int j = 0; j < 3; j++)
                        c[y][x][j] = c[y][x][j] * std::max(rest[y][x], 0.0f) + upper[0][y][x][j] + upper[1][y][x][j] + upper[2][y][x][j];
        if (oCv && (flags & 0x40) && s.Has(c0 + oCv + 8, 145 * 4)) {
            auto cv = [&](int r, int q, int j) { return adt[c0 + oCv + 8 + (r * 17 + q) * 4 + 2 - j] / 127.5f; };
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    for (int j = 0; j < 3; j++)
                        c[y][x][j] *= 0.25f * (cv(y, x, j) + cv(y + 1, x, j) + cv(y, x + 1, j) + cv(y + 1, x + 1, j));
        }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                for (int j = 0; j < 3; j++) col[((iy * 8 + y) * kAlbedoPx + ix * 8 + x) * 3 + j] = c[y][x][j];
    }
    if (!chunks) return false;

    // Liquids (MH2O): 256 headers (ofsInstances, layerCount, ofsAttributes), instances of 24 bytes.
    if (haveMh2o && s.Has(mh2o, 256 * 12)) {
        for (uint32_t ch = 0; ch < 256; ch++) {
            const uint32_t oi = s.Get<uint32_t>(mh2o + ch * 12), cnt = s.Get<uint32_t>(mh2o + ch * 12 + 4);
            for (uint32_t k = 0; oi && k < cnt && s.Has(mh2o + oi + k * 24, 24); k++) {
                const size_t in = mh2o + oi + k * 24;
                const uint16_t type = s.Get<uint16_t>(in);
                const uint8_t x0 = adt[in + 12], y0 = adt[in + 13], w = adt[in + 14], h = adt[in + 15];
                const uint32_t oe = s.Get<uint32_t>(in + 16);
                uint64_t bits = ~0ull;
                if (oe) {
                    bits = 0;
                    for (int i = 0; i < 8 && s.Has(mh2o + oe + i, 1); i++) bits |= uint64_t(adt[mh2o + oe + i]) << (8 * i);
                }
                const int8_t cat = int8_t(type < ctx.liquidCategory.size() ? std::min<uint8_t>(ctx.liquidCategory[type], 3) : 0);
                uint32_t j = 0;
                for (uint32_t r = y0; r < uint32_t(y0) + h; r++)
                    for (uint32_t q = x0; q < uint32_t(x0) + w; q++, j++)
                        if (r < 8 && q < 8 && j < 64 && (bits >> j & 1)) liq[((ch / 16) * 8 + r) * kAlbedoPx + (ch % 16) * 8 + q] = cat;
            }
        }
    }

    std::vector<float> img(size_t(kAlbedoPx) * kAlbedoPx * 4);
    float sum[3] = {};
    for (size_t i = 0; i < size_t(kAlbedoPx) * kAlbedoPx; i++) {
        const float* rgb = liq[i] >= 0 ? kLiquid[liq[i]] : &col[i * 3];
        for (int j = 0; j < 3; j++) {
            img[i * 4 + j] = std::min(std::max(rgb[j], 0.0f), 1.0f) * 255.0f;
            sum[j] += img[i * 4 + j];
        }
        img[i * 4 + 3] = 255.0f;
    }
    for (int j = 0; j < 3; j++) out.mean[j] = uint8_t(sum[j] / (kAlbedoPx * kAlbedoPx) + 0.5f);
    Encode(std::move(img), kAlbedoPx, kAlbedoLevels, false, out.albedo);

    // Normals at the odd vertices (texel centres), +-1 vertex = 8.33 yd baseline.
    const float sp = kTile / 128.0f;
    std::vector<float> nimg(size_t(kNormalPx) * kNormalPx * 4);
    for (uint32_t y = 0; y < kNormalPx; y++)
        for (uint32_t x = 0; x < kNormalPx; x++) {
            const int r = 2 * y + 1, q = 2 * x + 1;
            float nx = (hgt[(r + 1) * 129 + q] - hgt[(r - 1) * 129 + q]) / (2 * sp);
            float ny = (hgt[r * 129 + q + 1] - hgt[r * 129 + q - 1]) / (2 * sp);
            const float len = std::sqrt(nx * nx + ny * ny + 1.0f);
            nx /= len; ny /= len;
            bool wet = false;
            for (int k = 0; k < 4; k++) wet |= liq[(2 * y + k / 2) * kAlbedoPx + 2 * x + k % 2] >= 0;
            if (wet) nx = ny = 0.0f;
            float* o = &nimg[(y * kNormalPx + x) * 4];
            o[0] = 0.0f; o[1] = (ny * 0.5f + 0.5f) * 255.0f; o[2] = 0.0f; o[3] = (nx * 0.5f + 0.5f) * 255.0f;
        }
    Encode(std::move(nimg), kNormalPx, kNormalLevels, true, out.normal);
    return true;
}

} // namespace FarLandBake
