#include "WoW/Source/WorldClient/TerrainSunShadow.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "WoW/Source/WorldClient/FarTrees.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/ShadowQuery.h"

namespace TerrainSunShadow {

namespace {

constexpr int kSize = 256;                  // coarse texels per side, one per WDL outer vertex
constexpr float kCell = 33.333332f;         // WDL outer vertex spacing (MapLowDetail.cpp)
constexpr float kGridOrigin = 512.0f;       // 17066.666 / kCell: world x/y 0 in WDL vertices
// shadowMaskFine: the ADT level, one texel per MCVT outer vertex (SMOLTILE_SIZE, 8 per WDL cell), +-1067 yd.
constexpr int kFineSize = 512;
constexpr float kFineCell = 4.1666665f;      // SMOLTILE_SIZE (WorldConstants.h, not constexpr)
constexpr float kFineOrigin = 4096.0f;       // HALF_WORLD_SIZE / SMOLTILE_SIZE
constexpr int kFinePerCoarse = 8;
constexpr int kFineRecentre = 32;           // fine texels (133 yd) of drift before a rebuild
constexpr float kFineBlendUv = 0.05f;       // the fine level fades into the coarse over its outer 5% (107 yd)
constexpr uint32_t kChunkPollMs = 1000;     // how often the window's loaded ADT chunks are recounted
constexpr float kNone = -30000.0f;          // no land (no WDL tile, an ADT hole), and "nothing blocks the light"
constexpr float kSoftYards = 16.0f;         // fully dark this far under the lit height
constexpr int kRecentre = 16;               // texels the camera may drift before a rebuild
constexpr float kTurnCos = 0.99999f;        // ~0.26 degrees of light turn before a rebuild
constexpr float kMarchYards = 8000.0f;      // the CPU test's reach
constexpr float kTopOfWorld = 4000.0f;      // no land above this
constexpr uint32_t kFadeMs = 1000;          // vfog38 (audit M2): a rebuild cross-fades from the shown heights this long
constexpr uint32_t kFadeStepMs = 33;        // re-upload at most this often while fading
constexpr float kSnapCos = 0.99f;           // ~8 degrees of turn at once (debugHour, a time jump): no fade
constexpr float kUnset = -1e30f;            // from: not in the old window
// farTreesShadow: the far trees as crowns in the land heights, rasterised off the main thread (MakeTrees) on grids
// centred on the camera with a margin, so the builds only take max(land, crown) per texel.
constexpr int kTreeRecentre = 16;           // fine texels (67 yd) of drift before a new raster
constexpr int kTreeMargin = kFineRecentre + 2 * kTreeRecentre;   // the fine window's drift + the raster's
constexpr int kTreeFine = kFineSize + 2 + 2 * kTreeMargin;
constexpr int kTreeCoarse = kSize + 2 * (kRecentre + 2);
constexpr float kForest = 0.5f;             // a coarse cell takes its crowns' top once they cover this much of it

// One nested level (clipmap ring): a square window of lit heights on a grid of `cell` yards, r/c down world x/y.
struct Level {
    int size;
    float cell, origin;
    bool clampBorder;           // the coarse level: border texels "nothing blocks", so the clamp past it is lit
    std::vector<float> lit;     // the last build
    std::vector<float> from;    // the shown heights when it was built (same window), the fade's start
    std::vector<float> shown;   // lerp(from, lit, fade): what the texture holds
    std::vector<uint32_t> texels;
    CGxTex* tex = nullptr;
    int r0 = 0, c0 = 0;
    uint32_t fadeStart = 0, lastUpload = 0;
    bool fading = false, built = false;

    Level(int n, float cellYards, float gridOrigin, bool clamp)
        : size(n), cell(cellYards), origin(gridOrigin), clampBorder(clamp), lit(n * n), from(n * n), shown(n * n), texels(n * n) {}
};

CVar* s_cvar;
CVar* s_maskCvar;       // shadowMask (Legion survey 2.7)
CVar* s_softCvar;       // shadowSoft
CVar* s_fineCvar;       // shadowMaskFine
CVar* s_debugCvar;      // shadowDebug
Level s_coarse(kSize, kCell, kGridOrigin, true);
Level s_fine(kFineSize, kFineCell, kFineOrigin, false);
// The fine level's land, (kFineSize + 2)^2 from (r0 - 1, c0 - 1): ADT heights where loaded, WDL beyond, kNone in holes.
std::vector<float> s_fineLand((kFineSize + 2) * (kFineSize + 2));
int s_fineChunks;       // loaded ADT chunks over the fine window at its build
uint32_t s_finePoll;
float s_fineCam[2];     // the camera at the last Update, for the fine window
bool s_fineOn;          // the fine level is current (Visibility uses it)
float s_light[3];
// The trees' rasters (MakeTrees): crown tops on the fine grid, forest tops on the coarse, kNone where there are none.
struct Trees {
    const void* wdl = nullptr;
    uint32_t loads = 0;
    int fr0 = 0, fc0 = 0, cr0 = 0, cc0 = 0;
    std::vector<float> fine, coarse;
};
Trees s_trees;
std::future<Trees> s_treeJob;
// shadow audit P2: the fine level is built on a worker (MakeFine) from a land snapshot taken here, and a rebuild fades
// in the shader from the previous texture (s_finePrev, its old window's row) instead of re-blending and re-uploading.
struct FineJob {
    int r0 = 0, c0 = 0, chunks = 0;
    bool fade = false;
    std::vector<float> land, lit;
    std::vector<uint32_t> texels;
};
std::future<FineJob> s_fineJob;
bool s_fineStale;               // the coarse level or the window's chunks changed since the last snapshot
CGxTex* s_finePrev;
float s_finePrevRow[4];
uint32_t s_treeGen;             // FarTrees::CanopyGeneration at the last snapshot
float s_treeCam[2], s_treeNear;
bool s_builtTrees;              // the shown heights hold a tree raster
const void* s_wdl;
uint32_t s_loads;       // g_lowDetailLoads at the build (audit L10: a new WDL can reuse the freed one's address)

float Vertex(int r, int c) {
    if (r < 0 || c < 0 || r > 1024 || c > 1024) {
        return kNone;
    }
    const int tr = std::min(r >> 4, 63), tc = std::min(c >> 4, 63);
    const CMapAreaLow* area = CMap::s_lowDetail.m_areas[tr * 64 + tc];
    if (!area || !area->m_heights) {
        return kNone;
    }
    return area->m_heights[(r - tr * 16) * 17 + (c - tc * 16)];
}

// Bilinear over the WDL outer vertices; r runs down world x, c down world y (CMap::BuildLowDetailGridVertices).
float HeightAt(float r, float c) {
    const float fr = std::floor(r), fc = std::floor(c);
    const int ir = static_cast<int>(fr), ic = static_cast<int>(fc);
    const float h00 = Vertex(ir, ic), h01 = Vertex(ir, ic + 1), h10 = Vertex(ir + 1, ic), h11 = Vertex(ir + 1, ic + 1);
    if (h00 == kNone || h01 == kNone || h10 == kNone || h11 == kNone) {
        return std::max(std::max(h00, h01), std::max(h10, h11));   // a coast of missing tiles: the highest known
    }
    const float tr = r - fr, tc = c - fc;
    return (h00 * (1 - tc) + h01 * tc) * (1 - tr) + (h10 * (1 - tc) + h11 * tc) * tr;
}

// The loaded ADT chunk holding fine (MCVT outer) vertex (r, c), r down world x; nullptr if none.
const CMapChunk* ChunkAt(int r, int c) {
    if (r < 0 || c < 0 || r >= 8192 || c >= 8192) {
        return nullptr;
    }
    const int cy = r >> 3, cx = c >> 3;     // CMap::GetChunkAtPosition's chunkY (from x) and chunkX (from y)
    const CMapArea* area = CMap::m_areaTable[64 * (cy >> 4) + (cx >> 4)];
    const CMapChunk* chunk = area ? area->m_mapChunks[(cx & 15) + 16 * (cy & 15)] : nullptr;
    return chunk && chunk->m_height && chunk->m_header ? chunk : nullptr;
}

// Fine vertex (r, c): its MCVT outer height (row r & 7, col c & 7: index row * 17 + col), kNone when the cell it
// starts is a hole (CMap::GetHeightTerrain's low-res mask), the WDL where no ADT is loaded.
float FineVertex(int r, int c) {
    const CMapChunk* chunk = ChunkAt(r, c);
    if (!chunk) {
        return HeightAt(static_cast<float>(r) / kFinePerCoarse, static_cast<float>(c) / kFinePerCoarse);
    }
    const int row = r & 7, col = c & 7;
    if (chunk->m_header->m_holes & CMap::s_holeMask[4 * (row >> 1) + (col >> 1)]) {
        return kNone;
    }
    return chunk->m_height[row * 17 + col] + chunk->m_topLeftCoords.z;
}

// Bilinear over a fine land snapshot (FillFineLand's layout from (r0 - 1, c0 - 1)) at absolute fine grid (r, c), which
// must lie in the window +-1 (clamped to it: the worker may not read the WDL). A corner in a hole: the nearest
// corner, so a hole lets the light through about where it is.
float FineHeightAt(const float* land, int r0, int c0, float r, float c) {
    constexpr int n = kFineSize + 2;
    const float fr = std::clamp(std::floor(r), r0 - 1.0f, r0 + n - 3.0f), fc = std::clamp(std::floor(c), c0 - 1.0f, c0 + n - 3.0f);
    const int ir = static_cast<int>(fr) - r0 + 1, ic = static_cast<int>(fc) - c0 + 1;
    const float* h = &land[ir * n + ic];
    const float tr = std::clamp(r - fr, 0.0f, 1.0f), tc = std::clamp(c - fc, 0.0f, 1.0f);
    if (h[0] == kNone || h[1] == kNone || h[n] == kNone || h[n + 1] == kNone) {
        return h[(tr >= 0.5f ? n : 0) + (tc >= 0.5f ? 1 : 0)];
    }
    return (h[0] * (1 - tc) + h[1] * tc) * (1 - tr) + (h[n] * (1 - tc) + h[n + 1] * tc) * tr;
}

// A coarse level's built lit heights (window at r0, c0) at absolute coarse grid (r, c), kNone outside the window.
float CoarseLitAt(const float* lit, int r0, int c0, float r, float c) {
    const float fr = r - r0, fc = c - c0;
    const int ir = static_cast<int>(std::floor(fr)), ic = static_cast<int>(std::floor(fc));
    if (ir < 0 || ic < 0 || ir + 1 >= kSize || ic + 1 >= kSize) {
        return kNone;
    }
    const float* h = &lit[ir * kSize + ic];
    const float tr = fr - ir, tc = fc - ic;
    return (h[0] * (1 - tc) + h[1] * tc) * (1 - tr) + (h[kSize] * (1 - tc) + h[kSize + 1] * tc) * tr;
}

int CountFineChunks(int r0, int c0) {
    int n = 0;
    for (int r = r0 - 1; r <= r0 + kFineSize; r += 8) {
        for (int c = c0 - 1; c <= c0 + kFineSize; c += 8) {
            n += ChunkAt(r, c) != nullptr;
        }
    }
    return n;
}

void FillFineLand(int r0, int c0, std::vector<float>& land) {
    constexpr int n = kFineSize + 2;
    land.resize(n * n);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            land[i * n + j] = FineVertex(r0 - 1 + i, c0 - 1 + j);
        }
    }
}

uint16_t Half(float f) {    // normal range only (|f| <= 30000, rounded to nearest)
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 112;
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    const uint32_t bits = (static_cast<uint32_t>(exponent) << 10) + (((x & 0x7FFFFFu) + 0x1000u) >> 13);
    return static_cast<uint16_t>(sign | std::min(bits, 0x7BFFu));
}

float HalfToFloat(uint16_t h) {    // Half's output: normal or zero
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t x = exponent ? (static_cast<uint32_t>(h & 0x8000u) << 16) | ((exponent + 112) << 23) | (static_cast<uint32_t>(h & 0x3FFu) << 13) : 0u;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// shadow audit V2: R = the lit height in fp16, G = what that rounding lost (fp16 steps 1 yd past z 1024, 2 yd past 2048);
// the shaders sample R + G, which bilinear filtering keeps exact.
uint32_t Pack(float v) {
    const uint16_t hi = Half(v);
    return hi | static_cast<uint32_t>(Half(v - HalfToFloat(hi))) << 16;
}

void Callback(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t, void* user, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = static_cast<Level*>(user)->texels.data();
        stride = width * 4;
    }
}

// lit(p) = max over the land toward the light of (height - rise along the way): the lowest z the light reaches.
// Swept one texel line at a time from the light's side: lit(p) = max(H(q), lit(q)) - drop, q = p one step toward the
// light along the major axis (the minor offset |off| <= 1 keeps q in the previous line, already done). land(r, c) is
// the height at absolute grid (r, c); outside(r, c) the lit height where q leaves the window (kNone: nothing blocks).
template <class Land, class Outside>
void Build(float* lit, int n, float cell, int r0, int c0, const float L[3], Land land, Outside outside) {
    const float h = std::sqrt(L[0] * L[0] + L[1] * L[1]);
    const float rise = std::max(L[2], 0.005f) / std::max(h, 1e-4f);    // tan(elevation)
    const float dR = h > 1e-4f ? -L[0] / h : 0.0f;    // grid r/c grow as world x/y fall
    const float dC = h > 1e-4f ? -L[1] / h : 1.0f;
    const bool rowsMajor = std::fabs(dR) >= std::fabs(dC);
    const float m = std::max(std::fabs(dR), std::fabs(dC));
    const int step = (rowsMajor ? dR : dC) > 0 ? 1 : -1;
    const float off = (rowsMajor ? dC : dR) / m;
    const float drop = cell / m * rise;
    auto at = [&](int a, int b) -> float& { return rowsMajor ? lit[a * n + b] : lit[b * n + a]; };

    for (int k = 0; k < n; k++) {
        const int a = step > 0 ? n - 1 - k : k;
        const int na = a + step;
        for (int b = 0; b < n; b++) {
            const float nb = b + off;
            const float qr = rowsMajor ? static_cast<float>(r0 + na) : r0 + nb;
            const float qc = rowsMajor ? c0 + nb : static_cast<float>(c0 + na);
            float past;
            const int b0 = static_cast<int>(std::floor(nb));
            if (na >= 0 && na < n && b0 >= 0 && b0 + 1 < n) {
                const float t = nb - b0;
                past = at(na, b0) * (1 - t) + at(na, b0 + 1) * t;
            } else if (na >= 0 && na < n && b0 >= 0 && b0 < n) {
                past = at(na, b0);
            } else {
                past = outside(qr, qc);
            }
            at(a, b) = std::max(std::max(land(qr, qc), past) - drop, kNone);
        }
    }
}

// The shown heights at fade weight w into the texels.
void Blend(Level& lv, float w) {
    const int n = lv.size;
    for (int i = 0; i < n * n; i++) {
        const int r = i / n, c = i % n;
        const bool border = lv.clampBorder && (r == 0 || c == 0 || r == n - 1 || c == n - 1);
        lv.shown[i] = lv.from[i] + (lv.lit[i] - lv.from[i]) * w;
        lv.texels[i] = Pack(border ? kNone : lv.shown[i]);
    }
}

// Moves the level's window to (r, c) keeping what it showed for the fade (snap: no fade), then builds it.
template <class BuildFn>
void Rebuild(Level& lv, int r, int c, bool fade, uint32_t now, BuildFn build) {
    const int n = lv.size;
    fade = fade && lv.built && lv.tex;
    const int dr = r - lv.r0, dc = c - lv.c0;
    for (int i = 0; i < n * n; i++) {
        const int a = i / n + dr, b = i % n + dc;
        lv.from[i] = fade && a >= 0 && a < n && b >= 0 && b < n ? lv.shown[a * n + b] : kUnset;
    }
    lv.r0 = r;
    lv.c0 = c;
    build();
    for (int i = 0; i < n * n; i++) {
        if (lv.from[i] == kUnset) {
            lv.from[i] = lv.lit[i];
        }
    }
    lv.fading = fade;
    lv.fadeStart = now;
    Blend(lv, fade ? 0.0f : 1.0f);
}

// Steps the level's fade and uploads it when it changed (rebuilt or fading). False: no texture.
bool Upload(Level& lv, bool rebuilt, uint32_t now, const char* name) {
    bool upload = rebuilt;
    if (!rebuilt && lv.fading && now - lv.lastUpload >= kFadeStepMs) {
        const float w = std::min(static_cast<float>(now - lv.fadeStart) / kFadeMs, 1.0f);
        lv.fading = w < 1.0f;
        Blend(lv, w);
        upload = true;
    }
    if (upload) {
        lv.lastUpload = now;
        if (!lv.tex) {
            CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1, 0, 0, 0);
            g_theGxDevicePtr->TexCreate(GxTex_2d, lv.size, lv.size, 0, GxTex_Gr1616F, GxTex_Gr1616F, flags, &lv, Callback, name, lv.tex);
        }
        if (lv.tex) {
            GxTexUpdate(lv.tex, 0, 0, lv.size, lv.size, 1);
        }
        lv.built = lv.tex != nullptr;
    }
    return lv.tex != nullptr;
}

void Row(const Level& lv, float row[4], float w) {
    row[0] = (lv.origin - lv.c0 + 0.5f) / lv.size;
    row[1] = (lv.origin - lv.r0 + 0.5f) / lv.size;
    row[2] = 1.0f / (lv.cell * lv.size);
    row[3] = w;
}

bool FineWanted() {
    return (!s_fineCvar || s_fineCvar->m_intValue != 0) && MaskWanted();
}

void ReleaseLevel(Level& lv) {
    if (lv.tex) {
        GxTexDestroy(lv.tex);
        lv.tex = nullptr;
    }
    lv.built = false;
}

// Trees nearer than this (yd, flat) to the camera are left to the shadow cascades (the real models, and
// FarTrees::DrawShadow's impostors): their reach less the camera's drift between a raster and the fine level's
// rebuild. Past it a cascade and the lit heights may both shade a spot, which the shadow mask merges, never doubles.
float TreesNear() {
    const float reach = CShadowCache::s_shadowMode > SHADOW_MEDIUM ? CShadowCache::s_extShadowBands[2].m_radius : 0.0f;
    return std::max(reach - kTreeMargin * kFineCell * 1.4142f, 0.0f);
}

// Worker thread: nothing here touches the world. Fine: every crown (at least its nearest vertex) at the tree's top.
// Coarse: one 33 yd cell is too coarse for a lone tree, so a cell rises to its crowns' top only where they cover
// kForest of it (a forest). ponytail: coverage threshold, no partial heights.
Trees MakeTrees(std::vector<FarTrees::Canopy> trees, float camX, float camY, float near_, const void* wdl, uint32_t loads) {
    Trees out;
    out.wdl = wdl;
    out.loads = loads;
    out.fr0 = static_cast<int>(std::floor(kFineOrigin - camX / kFineCell)) - kTreeFine / 2;
    out.fc0 = static_cast<int>(std::floor(kFineOrigin - camY / kFineCell)) - kTreeFine / 2;
    out.cr0 = static_cast<int>(std::floor(kGridOrigin - camX / kCell)) - kTreeCoarse / 2;
    out.cc0 = static_cast<int>(std::floor(kGridOrigin - camY / kCell)) - kTreeCoarse / 2;
    out.fine.assign(kTreeFine * kTreeFine, kNone);
    out.coarse.assign(kTreeCoarse * kTreeCoarse, 0.0f);
    std::vector<float> top(kTreeCoarse * kTreeCoarse, kNone);
    const float near2 = near_ * near_;
    constexpr int n = kTreeFine;
    for (const FarTrees::Canopy& t : trees) {
        const float dx = t.x - camX, dy = t.y - camY;
        if (dx * dx + dy * dy <= near2) {
            continue;
        }
        const int r = static_cast<int>(std::lround(kGridOrigin - t.x / kCell)) - out.cr0;
        const int c = static_cast<int>(std::lround(kGridOrigin - t.y / kCell)) - out.cc0;
        if (r >= 0 && c >= 0 && r < kTreeCoarse && c < kTreeCoarse) {
            out.coarse[r * kTreeCoarse + c] += 3.1415927f * t.radius * t.radius;
            top[r * kTreeCoarse + c] = std::max(top[r * kTreeCoarse + c], t.top);
        }
        const float fr = kFineOrigin - t.x / kFineCell - out.fr0, fc = kFineOrigin - t.y / kFineCell - out.fc0;
        const float rr = t.radius / kFineCell;
        if (fr + rr < 0 || fc + rr < 0 || fr - rr > n - 1 || fc - rr > n - 1) {
            continue;
        }
        const int i0 = std::max(static_cast<int>(std::ceil(fr - rr)), 0), i1 = std::min(static_cast<int>(std::floor(fr + rr)), n - 1);
        const int j0 = std::max(static_cast<int>(std::ceil(fc - rr)), 0), j1 = std::min(static_cast<int>(std::floor(fc + rr)), n - 1);
        bool any = false;
        for (int i = i0; i <= i1; i++) {
            for (int j = j0; j <= j1; j++) {
                if ((i - fr) * (i - fr) + (j - fc) * (j - fc) <= rr * rr) {
                    out.fine[i * n + j] = std::max(out.fine[i * n + j], t.top);
                    any = true;
                }
            }
        }
        const int ni = static_cast<int>(std::lround(fr)), nj = static_cast<int>(std::lround(fc));
        if (!any && ni >= 0 && nj >= 0 && ni < n && nj < n) {
            out.fine[ni * n + nj] = std::max(out.fine[ni * n + nj], t.top);
        }
    }
    for (size_t i = 0; i < out.coarse.size(); i++) {
        out.coarse[i] = out.coarse[i] >= kForest * kCell * kCell ? top[i] : kNone;
    }
    return out;
}

bool TreesValid() {
    return !s_trees.fine.empty() && s_trees.wdl == CMap::s_lowDetail.m_data && s_trees.loads == g_lowDetailLoads;
}

// Main thread, every Update: takes a finished raster, starts the next when the trees changed or the camera drifted.
// Its share: the snapshot (FarTrees::Canopies, about 0.2 ms at 30k trees) on the frame a raster starts. True when a
// raster arrived and the shown heights hold none yet (the first after a map load or farTreesShadow on): build now;
// later rasters wait for the next build (drift or light turn), so the trees add no builds of their own.
bool PollTrees(const float camPos[3]) {
    bool first = false;
    if (s_treeJob.valid() && s_treeJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            s_trees = s_treeJob.get();
        } catch (...) {     // shadow audit C1: e.g. bad_alloc on Wow32; no trees rather than a rethrow into the frame
            s_trees = Trees();
        }
        first = !s_builtTrees && TreesValid();
    }
    if (s_treeJob.valid()) {
        return false;
    }
    const uint32_t gen = FarTrees::CanopyGeneration();
    const float near_ = TreesNear();
    const float drift = kTreeRecentre * kFineCell;
    if (gen == s_treeGen && near_ == s_treeNear && std::fabs(camPos[0] - s_treeCam[0]) <= drift && std::fabs(camPos[1] - s_treeCam[1]) <= drift) {
        return first;
    }
    s_treeGen = gen;
    s_treeNear = near_;
    s_treeCam[0] = camPos[0];
    s_treeCam[1] = camPos[1];
    if (gen == 0) {     // farTrees or farTreesShadow off: drop the raster, and the trees with the next build
        s_trees = Trees();
        return s_builtTrees;
    }
    std::vector<FarTrees::Canopy> trees;
    FarTrees::Canopies(trees);
    s_treeJob = std::async(std::launch::async, MakeTrees, std::move(trees), camPos[0], camPos[1], near_, CMap::s_lowDetail.m_data, g_lowDetailLoads);
    return first;
}

// The coarse land with the forests on it (absolute grid r, c; nearest cell).
float CoarseLandAt(float r, float c) {
    const float h = HeightAt(r, c);
    if (s_trees.coarse.empty()) {
        return h;
    }
    const int ir = static_cast<int>(std::lround(r)) - s_trees.cr0, ic = static_cast<int>(std::lround(c)) - s_trees.cc0;
    return ir < 0 || ic < 0 || ir >= kTreeCoarse || ic >= kTreeCoarse ? h : std::max(h, s_trees.coarse[ir * kTreeCoarse + ic]);
}

// The crowns over a fine land snapshot (FillFineLand's window): one max a texel, about 0.1 ms, on the fine level's builds only.
void ApplyFineTrees(int r0, int c0, std::vector<float>& fineLand) {
    if (!TreesValid()) {
        return;
    }
    constexpr int n = kFineSize + 2;
    const int di = r0 - 1 - s_trees.fr0, dj = c0 - 1 - s_trees.fc0;
    const int i0 = std::max(0, -di), i1 = std::min(n, kTreeFine - di), j0 = std::max(0, -dj), j1 = std::min(n, kTreeFine - dj);
    for (int i = i0; i < i1; i++) {
        float* land = &fineLand[i * n];
        const float* crown = &s_trees.fine[(i + di) * kTreeFine + dj];
        for (int j = j0; j < j1; j++) {
            land[j] = std::max(land[j], crown[j]);
        }
    }
}

// Worker thread: the fine level's sweep over its land snapshot, its light-side edge from a copy of the coarse lit heights.
FineJob MakeFine(FineJob job, std::vector<float> coarse, int cr0, int cc0, std::array<float, 3> L) {
    job.lit.resize(kFineSize * kFineSize);
    Build(job.lit.data(), kFineSize, kFineCell, job.r0, job.c0, L.data(),
        [&](float qr, float qc) { return FineHeightAt(job.land.data(), job.r0, job.c0, qr, qc); },
        [&](float qr, float qc) { return CoarseLitAt(coarse.data(), cr0, cc0, qr / kFinePerCoarse, qc / kFinePerCoarse); });
    job.texels.resize(job.lit.size());
    for (size_t i = 0; i < job.lit.size(); i++) {
        job.texels[i] = Pack(job.lit[i]);
    }
    return job;
}

// Main thread: takes a finished fine build (the previous texture fades out in the shader), starts the next one when
// it is due and none is running or fading.
void UpdateFine(bool due, bool fade, uint32_t now) {
    if (s_fineJob.valid() && s_fineJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        FineJob job;
        try {
            job = s_fineJob.get();
        } catch (...) {     // shadow audit C1
            s_fineStale = true;
        }
        if (!job.lit.empty()) {
            s_fine.fading = job.fade && s_fine.tex;
            if (s_fine.fading) {
                Row(s_fine, s_finePrevRow, 0.0f);
                std::swap(s_fine.tex, s_finePrev);
                s_fine.fadeStart = now;
            }
            s_fine.r0 = job.r0;
            s_fine.c0 = job.c0;
            s_fineChunks = job.chunks;
            s_fineLand.swap(job.land);
            s_fine.lit.swap(job.lit);
            s_fine.texels.swap(job.texels);
            Upload(s_fine, true, now, "TerrainSunShadowFine");
            s_fineOn = s_fine.built;
        }
    }
    if (s_fine.fading && now - s_fine.fadeStart >= kFadeMs) {
        s_fine.fading = false;
    }
    if (!due || s_fineJob.valid() || s_fine.fading) {
        return;
    }
    FineJob job;
    job.r0 = static_cast<int>(std::floor(kFineOrigin - s_fineCam[0] / kFineCell)) - kFineSize / 2;
    job.c0 = static_cast<int>(std::floor(kFineOrigin - s_fineCam[1] / kFineCell)) - kFineSize / 2;
    job.fade = fade && s_fineOn;
    FillFineLand(job.r0, job.c0, job.land);     // world data: the main thread's share
    ApplyFineTrees(job.r0, job.c0, job.land);
    job.chunks = CountFineChunks(job.r0, job.c0);
    s_fineStale = false;
    s_fineJob = std::async(std::launch::async, MakeFine, std::move(job), s_coarse.lit, s_coarse.r0, s_coarse.c0, std::array<float, 3> { s_light[0], s_light[1], s_light[2] });
}

}  // namespace

void RegisterCVars() {
    s_cvar = CVar::Register("gfxTerrainShadow", "The land (near terrain and the far WDL horizon) blocks the sun for the volumetric fog's sun shafts and glow and for the sun glare: 1 = on (default), 0 = off", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_maskCvar = CVar::Register("shadowMask", "Mountains (near terrain and the far WDL horizon) cast the sun's shadow onto the world: terrain, buildings and models (needs gfxTerrainShadow): 1 = on (default), 0 = off", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_softCvar = CVar::Register("shadowSoft", "Softness of the mountains' shadow edge (shadowMask), 0 = hard .. 4 = very soft, default 1", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_debugCvar = CVar::Register("shadowDebug", "Shows the sun shadow through the shadow mask pass (any shadowMask): 1 = which map each pixel reads (yellow unit map, green band 0, blue band 1, orange band 2, red none), 2 = the sun's visibility in grey (cascades and land), 3 = magenta where no cascade is read, 0 = off (default)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_fineCvar = CVar::Register("shadowMaskFine", "The mountains' shadow (shadowMask) from the loaded terrain's own heights (4 yd) within 1 km of the camera, the far WDL (33 yd) beyond: 1 = on (default), 0 = WDL only", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

int DebugMode() {
    return s_debugCvar ? std::clamp(s_debugCvar->m_intValue, 0, 3) : 0;
}

const float* LightDir() {
    return s_light;
}

bool MaskWanted() {
    return (!s_maskCvar || s_maskCvar->m_intValue != 0) && Enabled();
}

float MaskSoftYards() {
    // shadowSoft 0..4: the land's penumbra, yards under the lit height to full dark (kSoftYards each); 0 = 2 yd.
    const float soft = s_softCvar ? s_softCvar->m_floatValue : 1.0f;
    return std::max(kSoftYards * std::clamp(soft, 0.0f, 4.0f), 2.0f);
}

bool Enabled() {
    return (!s_cvar || s_cvar->m_intValue != 0) && CMap::s_lowDetail.m_data != nullptr;
}

bool Update(const float trueLight[3], const float camPos[3], CGxTex*& tex, float row[4]) {
    tex = nullptr;
    // gfxSunShadows: the cascades' latched light (elevation 12-70 deg), so a mountain's shadow and a tree's next to it
    // have the same length and side; stock light otherwise. The latch moves in steps far under kTurnCos, so they
    // add up to a rebuild here (the land lags the cascades by at most that), and its 5 deg jumps fade (kSnapCos).
    C3Vector latched;
    float shadowLight[3];
    const bool useLatch = CShadowQuery::SunLatched(latched);
    if (useLatch) {
        shadowLight[0] = latched.x;
        shadowLight[1] = latched.y;
        shadowLight[2] = latched.z;
    }
    const float* toLight = useLatch ? shadowLight : trueLight;
    if (!Enabled() || toLight[2] <= 0.0f || !g_theGxDevicePtr) {
        s_fineOn = false;
        return false;
    }
    const int r = static_cast<int>(std::floor(kGridOrigin - camPos[0] / kCell)) - kSize / 2;
    const int c = static_cast<int>(std::floor(kGridOrigin - camPos[1] / kCell)) - kSize / 2;
    const float turn = toLight[0] * s_light[0] + toLight[1] * s_light[1] + toLight[2] * s_light[2];
    const uint32_t now = OsGetAsyncTimeMs();
    const bool sameMap = s_wdl == CMap::s_lowDetail.m_data && g_lowDetailLoads == s_loads;
    const bool trees = PollTrees(camPos);     // farTreesShadow
    const bool coarse = trees || !s_coarse.built || !s_coarse.tex || !sameMap || turn < kTurnCos || std::abs(r - s_coarse.r0) > kRecentre || std::abs(c - s_coarse.c0) > kRecentre;
    // vfog38 (audit M2): fade from the heights shown now, moved into the new window; a new map, the first build
    // or a big turn snaps. Texels the old window did not cover start at the new heights.
    const bool fade = sameMap && turn >= kSnapCos;
    if (coarse) {
        std::memcpy(s_light, toLight, sizeof(s_light));
        s_wdl = CMap::s_lowDetail.m_data;
        s_loads = g_lowDetailLoads;
        if (!TreesValid()) {
            s_trees = Trees();      // another map's
        }
        s_builtTrees = !s_trees.fine.empty();
        Rebuild(s_coarse, r, c, fade, now, [&] {
            Build(s_coarse.lit.data(), kSize, kCell, s_coarse.r0, s_coarse.c0, toLight, CoarseLandAt, [](float, float) { return kNone; });
        });
    }
    if (!Upload(s_coarse, coarse, now, "TerrainSunShadow")) {
        s_fineOn = false;
        return false;
    }

    // shadowMaskFine: the ADT level, rebuilt with the coarse (its light-side edge reads the coarse lit heights), on
    // its own drift, or when the window's loaded chunks changed (polled; ADTs stream in after a teleport).
    if (FineWanted()) {
        const int fr = static_cast<int>(std::floor(kFineOrigin - camPos[0] / kFineCell)) - kFineSize / 2;
        const int fc = static_cast<int>(std::floor(kFineOrigin - camPos[1] / kFineCell)) - kFineSize / 2;
        if (!fade && coarse) {
            s_fineOn = false;   // a new map or a snap: the old fine level is wrong until its rebuild arrives
        }
        s_fineStale = s_fineStale || coarse;
        if (!s_fineStale && s_fine.built && now - s_finePoll >= kChunkPollMs) {
            s_finePoll = now;
            s_fineStale = CountFineChunks(s_fine.r0, s_fine.c0) != s_fineChunks;
        }
        s_fineCam[0] = camPos[0];
        s_fineCam[1] = camPos[1];
        UpdateFine(s_fineStale || !s_fine.built || !s_fineOn || std::abs(fr - s_fine.r0) > kFineRecentre || std::abs(fc - s_fine.c0) > kFineRecentre, fade, now);
    } else {
        s_fineOn = false;
    }

    tex = s_coarse.tex;
    Row(s_coarse, row, 1.0f / MaskSoftYards());     // vfog38 (audit L6): fog, water and caustics soften as the mask does
    return true;
}

bool Fine(CGxTex*& tex, float row[4], CGxTex*& from, float fromRow[4]) {
    from = nullptr;
    std::memset(fromRow, 0, 4 * sizeof(float));
    fromRow[3] = 1.0f;
    if (!s_fineOn || !FineWanted() || !s_fine.tex) {
        tex = nullptr;
        std::memset(row, 0, 4 * sizeof(float));
        return false;
    }
    tex = s_fine.tex;
    Row(s_fine, row, 1.0f / kFineBlendUv);
    if (s_fine.fading && s_finePrev) {
        from = s_finePrev;
        std::memcpy(fromRow, s_finePrevRow, 3 * sizeof(float));
        fromRow[3] = std::min(static_cast<float>(OsGetAsyncTimeMs() - s_fine.fadeStart) / kFadeMs, 1.0f);
    }
    return true;
}

float Visibility(const float pos[3], const float toward[3]) {
    if (!Enabled() || toward[2] <= 0.0f) {
        return 1.0f;
    }
    const float h = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1]);
    if (h < 1e-4f) {
        return 1.0f;
    }
    const float rise = toward[2] / h;
    const bool fine = s_fineOn && FineWanted();
    float s = kFineCell * 0.5f;
    while (s < kMarchYards) {
        const float z = pos[2] + s * rise;
        if (z > kTopOfWorld) {
            break;
        }
        const float x = pos[0] + toward[0] / h * s, y = pos[1] + toward[1] / h * s;
        // The fine level's heights (as the mask's texture) inside its window, the WDL beyond.
        const float fr = kFineOrigin - x / kFineCell - s_fine.r0, fc = kFineOrigin - y / kFineCell - s_fine.c0;
        const bool inFine = fine && fr >= 0.0f && fc >= 0.0f && fr < kFineSize && fc < kFineSize;
        const float land = inFine ? FineHeightAt(s_fineLand.data(), s_fine.r0, s_fine.c0, fr + s_fine.r0, fc + s_fine.c0) : HeightAt(kGridOrigin - x / kCell, kGridOrigin - y / kCell);
        if (land > z + 2.0f) {
            return 0.0f;
        }
        s += (inFine ? kFineCell : kCell) * 0.5f;
    }
    return 1.0f;
}

void Release() {
    // shadow audit M1: no raster left running into the CRT teardown (RendererShutdown calls this before it)
    if (s_treeJob.valid()) {
        s_treeJob.wait();
    }
    if (s_fineJob.valid()) {
        s_fineJob.wait();
    }
    ReleaseLevel(s_coarse);
    ReleaseLevel(s_fine);
    if (s_finePrev) {
        GxTexDestroy(s_finePrev);
        s_finePrev = nullptr;
    }
    s_fine.fading = false;
    s_fineOn = false;
}

}  // namespace TerrainSunShadow
