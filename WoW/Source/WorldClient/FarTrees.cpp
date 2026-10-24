// FARTREES: not in 12340. See FarTrees.h and fartrees/FARTREES-PLAN.md. The main thread never reads files or bakes:
// it asks FarTreesBake's worker for the ADTs within view distance and for each new tree model's atlas, and per frame
// takes at most kTilesPerFrame placement lists and one finished atlas (two texture creates).
#include "WoW/Source/WorldClient/FarTrees.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CFrustum.h"
#include "WoW/Source/WorldClient/CWorldMath.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FarLand.h"
#include "WoW/Source/WorldClient/FarTreesBake.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldView.h"

namespace FarTrees {

int32_t s_enabled = 0;
int32_t s_shadow = 1;

namespace {

constexpr float kMapC = 32.0f * 533.33333f, kTile = 533.33333f;
constexpr float kMinPixels = 2.0f;      // a picture smaller than this on screen is not drawn
constexpr float kDepthBias = 0.25f;     // x R: the impostor stays behind its real tree while that is still drawn (covers
                                        // the 4-view mix's depth error, about 0.2 R at the crown's edge)
constexpr float kFadeInMs = 1000.0f;    // a model whose atlas just arrived dissolves in over this
constexpr uint32_t kTilesPerFrame = 2;  // placement lists turned into vertex buffers per frame
constexpr float kKeepMargin = 600.0f;   // a tile this far past farclip is dropped
constexpr uint32_t kIdleMs = 10000;     // a model's atlas not drawn for this long may be dropped ...
constexpr size_t kBudget = (sizeof(void*) == 4 ? 128u : 512u) << 20;   // ... while all atlases (RAM + VRAM) pass this

struct Vertex { float p[3], t[2]; };   // GxVBF_PT
static_assert(sizeof(Vertex) == 20, "GxVBF_PT");

struct TexData { const std::vector<std::vector<uint8_t>>* levels; };

struct Model {
    std::string name;
    int state = 0;   // 0 asked, 1 ready, 2 failed, 3 dropped (Evict; asked again when drawn)
    FarTreesBake::ModelResult data;   // DXT levels kept for the device's latch (and a device reset)
    TexData albData, nrmData;
    CGxTex* alb = nullptr;
    CGxTex* nrm = nullptr;
    uint32_t readyMs = 0;
    uint32_t usedMs = 0;   // last drawn
};

struct Place { float x, y, z, yaw, scale, rotX, rotY; uint32_t model, uniqueId; float size = -1.0f; };
struct Segment { uint32_t model, first, count; CGxBuf* buf; };
struct Tile {
    bool ready = false;
    std::vector<Place> places;   // sorted by model
    std::vector<Segment> segs;
    CGxPool* pool = nullptr;
    float lo[3], hi[3];
};

std::string s_map;
std::deque<Model> s_models;   // deque: stable addresses for the latch callbacks
std::unordered_map<std::string, uint32_t> s_modelIndex;
std::unordered_map<int32_t, Tile> s_tiles;   // key tx * 64 + ty
std::unordered_set<uint32_t> s_seen;         // uniqueIds placed (a doodad crossing tiles is in several ADTs)

CGxShader* s_vs = nullptr;
CGxShader* s_ps = nullptr;
bool s_failed = false;
CGxShader* s_shVs = nullptr;   // farTreesShadow casters
CGxShader* s_shPs = nullptr;
bool s_shFailed = false;
uint32_t s_generation = 0;     // CanopyGeneration

void TexCallback(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t level, void* userArg, uint32_t& stride, const void*& texels) {
    if (command != GxTex_Latch) return;
    const auto* levels = static_cast<TexData*>(userArg)->levels;
    stride = (width >> 2) * 16 < 16 ? 16 : (width >> 2) * 16;
    texels = level < levels->size() ? (*levels)[level].data() : nullptr;
}

// The size class stock gives this doodad (CMapDoodadDef::UpdateBounds): the largest side of the world box of its
// rotated, scaled header box, against CWorldView::s_fadeSize. Returns the scale for class 2, -scale for class 3
// (FarTreesVS / ShadowVS: the sign picks the fade), 0 for the classes no impostor stands in for. Needs the model's box.
float SizeClass(Place& p) {
    if (p.size < 0.0f) {
        const float* b = s_models[p.model].data.box;
        CAaBox box(C3Vector(b[0], b[1], b[2]), C3Vector(b[3], b[4], b[5])), world;
        C44Matrix mat;   // as CMap::CreateDoodadDef, without the translation
        mat.RotateAroundZ(p.yaw * 6.2831853f);
        mat.RotateAroundY(p.rotY * 0.017453292f);
        mat.RotateAroundX(p.rotX * 0.017453292f);
        mat.Scale(p.scale);
        p.size = 0.0f;
        if (box.b.x <= box.t.x || box.b.y <= box.t.y || box.b.z <= box.t.z) {
            CWorldMath::TransformAABox(mat, box, world);
            p.size = std::fmax(std::fmax(world.t.x - world.b.x, world.t.y - world.b.y), world.t.z - world.b.z);
        }
    }
    if (p.size <= CWorldView::s_fadeSize[1] || p.size > CWorldView::s_fadeSize[3]) return 0.0f;
    return p.size > CWorldView::s_fadeSize[2] ? -p.scale : p.scale;
}

size_t Bytes(const Model& m) {
    size_t n = 0;
    for (const auto* set : { &m.data.albedo, &m.data.normal }) for (const auto& l : *set) n += l.size();
    return n * 2;   // the RAM copy for the latch and the texture
}

// Over kBudget: drop the atlases unused longest (not the placements' data: centre, radius and box stay for the
// canopies and SizeClass). Draw asks for a dropped one again when a tile in reach needs it (a cache read).
void Evict(uint32_t now) {
    size_t bytes = 0;
    for (const Model& m : s_models) if (m.state == 1) bytes += Bytes(m);
    while (bytes > kBudget) {
        Model* lru = nullptr;
        for (Model& m : s_models)
            if (m.state == 1 && now - m.usedMs > kIdleMs && (!lru || now - m.usedMs > now - lru->usedMs)) lru = &m;
        if (!lru) break;
        bytes -= Bytes(*lru);
        if (lru->alb) g_theGxDevicePtr->TexDestroy(lru->alb);
        if (lru->nrm) g_theGxDevicePtr->TexDestroy(lru->nrm);
        lru->alb = lru->nrm = nullptr;
        std::vector<std::vector<uint8_t>>().swap(lru->data.albedo);
        std::vector<std::vector<uint8_t>>().swap(lru->data.normal);
        lru->state = 3;
    }
}

// The pass's shader constants go back to what they were (from the device's shadow copy): stock keeps state there
// that it does not set every draw (PS c2 = CShaderEffect::s_fogColorAlphaRef, the band's VS constants, ...).
struct SavedConstants {
    float vs[11][4], ps[4][4];
    SavedConstants() {
        std::memcpy(vs, CGxDevice::s_shadowConstants[GxSh_Vertex ? 0 : 1].constants, sizeof(vs));
        std::memcpy(ps, CGxDevice::s_shadowConstants[GxSh_Pixel ? 0 : 1].constants, sizeof(ps));
    }
    void Restore() {
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, &vs[0][0], 11);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &ps[0][0], 4);
    }
};

void FreeTile(Tile& t) {
    if (!t.places.empty()) s_generation++;
    for (Segment& s : t.segs) if (s.buf) g_theGxDevicePtr->BufDestroy(s.buf);
    t.segs.clear();
    if (t.pool) g_theGxDevicePtr->PoolDestroy(t.pool);
    t.pool = nullptr;
    for (const Place& p : t.places) s_seen.erase(p.uniqueId);
}

void Reset() {
    FarTreesBake::Stop();
    for (auto& kv : s_tiles) FreeTile(kv.second);
    for (Model& m : s_models) {
        if (m.alb) g_theGxDevicePtr->TexDestroy(m.alb);
        if (m.nrm) g_theGxDevicePtr->TexDestroy(m.nrm);
    }
    s_tiles.clear();
    s_models.clear();
    s_modelIndex.clear();
    s_seen.clear();
    s_map.clear();
    s_generation++;
}

uint32_t ModelIndex(const std::string& name) {
    auto it = s_modelIndex.find(name);
    if (it != s_modelIndex.end()) return it->second;
    const uint32_t i = static_cast<uint32_t>(s_models.size());
    s_models.emplace_back();
    s_models.back().name = name;
    s_modelIndex.emplace(name, i);
    FarTreesBake::RequestModel(name);
    return i;
}

void TakeTile(FarTreesBake::TileResult& r) {
    if (r.map != s_map) return;
    auto it = s_tiles.find(r.tx * 64 + r.ty);
    if (it == s_tiles.end() || it->second.ready) return;
    Tile& t = it->second;
    t.ready = true;
    for (int k = 0; k < 3; ++k) { t.lo[k] = 1e30f; t.hi[k] = -1e30f; }
    for (const FarTreesBake::Place& p : r.places) {
        if (!s_seen.insert(p.uniqueId).second) continue;
        t.places.push_back({ p.x, p.y, p.z, p.yaw, p.scale, p.rot[2], p.rot[0], ModelIndex(p.model), p.uniqueId });
        // Bounds before the model is known: a generous 120 yd (size class 3 tops out at 100 yd) times the scale.
        const float r_ = 120.0f * p.scale, c[3] = { p.x, p.y, p.z };
        for (int k = 0; k < 3; ++k) { t.lo[k] = std::fmin(t.lo[k], c[k] - r_); t.hi[k] = std::fmax(t.hi[k], c[k] + r_); }
    }
    std::sort(t.places.begin(), t.places.end(), [](const Place& a, const Place& b) { return a.model < b.model; });
    for (uint32_t j = 0; j < t.places.size(); ++j) {
        if (t.segs.empty() || t.segs.back().model != t.places[j].model) t.segs.push_back({ t.places[j].model, j, 0, nullptr });
        t.segs.back().count++;
    }
    if (!t.places.empty()) s_generation++;
}

void TakeModel(FarTreesBake::ModelResult& r, uint32_t now) {
    auto it = s_modelIndex.find(r.model);
    if (it == s_modelIndex.end()) return;
    Model& m = s_models[it->second];
    m.data = std::move(r);
    if (!m.data.ok || m.data.albedo.empty()) { m.state = 2; return; }
    const uint32_t W = FarTreesBake::kFrames * m.data.framePx;
    m.albData.levels = &m.data.albedo;
    m.nrmData.levels = &m.data.normal;
    CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, W, W, 0, GxTex_Dxt5, GxTex_Dxt5, flags, &m.albData, TexCallback, "FarTreesA", m.alb);
    g_theGxDevicePtr->TexCreate(GxTex_2d, W, W, 0, GxTex_Dxt5, GxTex_Dxt5, flags, &m.nrmData, TexCallback, "FarTreesN", m.nrm);
    m.state = m.alb && m.nrm ? 1 : 2;
    m.readyMs = m.usedMs = now;
    if (m.state == 1) s_generation++;
}

bool Gpu() {
    if (s_failed) return false;
    if (!s_vs) {
        g_theGxDevicePtr->ShaderCreate(&s_vs, GxSh_Vertex, "Shaders\\Vertex", "FarTreesVS", 1);
        g_theGxDevicePtr->ShaderCreate(&s_ps, GxSh_Pixel, "Shaders\\Pixel", "FarTreesPS", 1);
    }
    if (!s_vs || !s_vs->Valid() || !s_ps || !s_ps->Valid()) {
        s_failed = true;   // no FarTrees shaders in custom.MPQ (or no shader path): no far trees
        return false;
    }
    return true;
}

bool GpuShadow() {
    if (s_shFailed) return false;
    if (!s_shVs) {
        g_theGxDevicePtr->ShaderCreate(&s_shVs, GxSh_Vertex, "Shaders\\Vertex", "FarTreesShadowVS", 1);
        g_theGxDevicePtr->ShaderCreate(&s_shPs, GxSh_Pixel, "Shaders\\Pixel", "FarTreesShadowPS", 1);
    }
    if (!s_shVs || !s_shVs->Valid() || !s_shPs || !s_shPs->Valid()) {
        s_shFailed = true;   // an older custom.MPQ without them: no far tree shadows in the cascades
        return false;
    }
    return true;
}

// Six vertices a tree (corners 0 1 2, 0 2 3).
void FillSegment(Tile& t, Segment& s) {
    auto* v = reinterpret_cast<Vertex*>(g_theGxDevicePtr->BufLock(s.buf));
    if (!v) return;
    static const float kCorner[6] = { 0, 1, 2, 0, 2, 3 };
    for (uint32_t j = s.first; j < s.first + s.count; ++j) {
        Place& pl = t.places[j];
        const float cls = SizeClass(pl);
        for (int k = 0; k < 6; ++k, ++v) {
            v->p[0] = pl.x; v->p[1] = pl.y; v->p[2] = pl.z;
            v->t[0] = kCorner[k] + pl.yaw * 0.999f;   // keep frac() below 1 so the corner survives
            v->t[1] = cls;   // scale, signed by the size class (0: not drawn)
        }
    }
    g_theGxDevicePtr->BufUnlock(s.buf, 0);
    s.buf->m_hasData = 1;
}

bool EnsureBuffers(Tile& t) {
    if (t.segs.empty()) return false;
    if (!t.pool) {
        t.pool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, static_cast<int32_t>(t.places.size() * 6 * sizeof(Vertex)), GxPoolHintBit_Unk0, "FarTrees");
        if (!t.pool) return false;
        for (Segment& s : t.segs) s.buf = g_theGxDevicePtr->BufCreate(t.pool, sizeof(Vertex), s.count * 6, 0);
    }
    for (Segment& s : t.segs)
        if (s.buf && s_models[s.model].state == 1 && !(s.buf->m_hasData && s.buf->m_valid)) FillSegment(t, s);
    return true;
}

// False when the box is wholly outside one side of the clip volume (clip = (p - cam) * M, row vectors).
bool BoxVisible(const float* M, const C3Vector& cam, const float* lo, const float* hi) {
    int out[5] = {};
    for (int i = 0; i < 8; ++i) {
        const float x = ((i & 1) ? hi[0] : lo[0]) - cam.x, y = ((i & 2) ? hi[1] : lo[1]) - cam.y, z = ((i & 4) ? hi[2] : lo[2]) - cam.z;
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = x * M[k] + y * M[4 + k] + z * M[8 + k] + M[12 + k];
        out[0] += c[0] < -c[3]; out[1] += c[0] > c[3]; out[2] += c[1] < -c[3]; out[3] += c[1] > c[3]; out[4] += c[3] <= 0.0f;
    }
    for (int k = 0; k < 5; ++k) if (out[k] == 8) return false;
    return true;
}

float TileDist2(int32_t tx, int32_t ty, float cx, float cy) {
    const float ylo = kMapC - (tx + 1) * kTile, yhi = kMapC - tx * kTile, xlo = kMapC - (ty + 1) * kTile, xhi = kMapC - ty * kTile;
    const float dx = std::fmax(std::fmax(xlo - cx, cx - xhi), 0.0f), dy = std::fmax(std::fmax(ylo - cy, cy - yhi), 0.0f);
    return dx * dx + dy * dy;
}

void Rgb(const CImVector& c, float* out) { out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f; }

} // namespace

void Draw() {
    if (!g_theGxDevicePtr) return;
    if (!s_enabled) {
        if (!s_map.empty() || !s_models.empty()) Reset();
        return;
    }
    // objectFade off draws every doodad to its chunk's cull: nothing to stand in for.
    if (!(CWorld::enables & Enable_Doodads) || !(CWorld::enables & Enable_NoFullAlpha) || !Gpu()) return;
    if (s_map != CMap::mapName) {
        Reset();
        s_map = CMap::mapName;
    }
    FarTreesBake::Start();

    const DayNight::DNInfo& dn = DayNight::g_dnInfo;
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float reach = dn.m_farClip;
    const uint32_t now = World::s_m2Scene ? World::s_m2Scene->m_time : 0;

    // Ask for the ADTs within reach, drop the ones well past it.
    const int32_t ctx = static_cast<int32_t>(std::floor((kMapC - cam.y) / kTile)), cty = static_cast<int32_t>(std::floor((kMapC - cam.x) / kTile));
    const int32_t span = static_cast<int32_t>(reach / kTile) + 1;
    for (int32_t tx = ctx - span; tx <= ctx + span; ++tx)
        for (int32_t ty = cty - span; ty <= cty + span; ++ty) {
            if (tx < 0 || ty < 0 || tx > 63 || ty > 63 || TileDist2(tx, ty, cam.x, cam.y) > reach * reach) continue;
            if (s_tiles.emplace(tx * 64 + ty, Tile()).second) FarTreesBake::RequestTile(s_map, tx, ty);
        }
    for (auto it = s_tiles.begin(); it != s_tiles.end();) {
        const float far_ = reach + kKeepMargin;
        if (TileDist2(it->first / 64, it->first % 64, cam.x, cam.y) > far_ * far_) {
            if (!it->second.ready) FarTreesBake::CancelTile(s_map, it->first / 64, it->first % 64);
            FreeTile(it->second);
            it = s_tiles.erase(it);
        }
        else ++it;
    }

    // The per-frame cap on the main thread's share of the work.
    FarTreesBake::TileResult tr;
    for (uint32_t i = 0; i < kTilesPerFrame && FarTreesBake::PopTile(tr); ++i) TakeTile(tr);
    FarTreesBake::ModelResult mr;
    if (FarTreesBake::PopModel(mr)) TakeModel(mr, now);
    static uint32_t s_evictMs = 0;
    if (now - s_evictMs > 1000) { s_evictMs = now; Evict(now); }

    C44Matrix vp;
    GxXformViewProjNativeTranspose(vp);
    vp = vp.Transpose();
    const float* M = reinterpret_cast<const float*>(&vp);

    float k[10][4] = {};
    std::memcpy(k, M, sizeof(float) * 16);
    k[4][0] = cam.x; k[4][1] = cam.y; k[4][2] = cam.z;
    k[7][0] = CWorldScene::s_fadeDistMin[2]; k[7][1] = CWorldScene::s_fadeDistMin[3];
    k[7][2] = CWorldScene::s_fadeRangeDefault[2]; k[7][3] = CWorldScene::s_fadeRangeDefault[3];
    float screenH = 1080.0f;   // window height in px (a world viewport smaller than the window only draws a few more)
    {
        CRect r;
        g_theGxDevicePtr->CapsWindowSize(r);
        if (r.maxY - r.minY > 1.0f) screenH = static_cast<float>(r.maxY - r.minY);
    }
    k[8][0] = std::fabs(g_theGxDevicePtr->m_projNative.b1) * screenH * 0.5f;
    k[8][1] = kMinPixels;
    const float fogStart = dn.m_fog.m_start, fogEnd = dn.m_fog.m_end;
    k[8][2] = fogStart;
    k[8][3] = fogEnd > fogStart + 0.01f ? 1.0f / (fogEnd - fogStart) : 0.0f;
    k[9][0] = -dn.m_light0.m_dir.x; k[9][1] = -dn.m_light0.m_dir.y; k[9][2] = -dn.m_light0.m_dir.z;
    k[9][3] = kDepthBias;
    float pc[4][4] = {};
    Rgb(dn.m_light0.m_ambient, pc[0]);
    Rgb(dn.m_light0.m_diffuse, pc[1]);
    Rgb(dn.m_fog.m_color, pc[2]);

    const float nearest = std::fmin(CWorldScene::s_fadeDistMin[2], CWorldScene::s_fadeDistMin[3]) - 4.0f;
    bool begun = false;
    SavedConstants saved;
    for (auto& kv : s_tiles) {
        Tile& t = kv.second;
        if (!t.ready || t.places.empty()) continue;
        float d2 = 0.0f, far2 = 0.0f;
        const float c[3] = { cam.x, cam.y, cam.z };
        for (int i = 0; i < 3; ++i) {
            const float dn_ = std::fmax(std::fmax(t.lo[i] - c[i], c[i] - t.hi[i]), 0.0f);
            const float df = std::fmax(std::fabs(t.lo[i] - c[i]), std::fabs(t.hi[i] - c[i]));
            d2 += dn_ * dn_; far2 += df * df;
        }
        if (d2 > reach * reach || far2 < nearest * nearest || !BoxVisible(M, cam, t.lo, t.hi)) continue;
        if (!EnsureBuffers(t)) continue;

        for (const Segment& s : t.segs) {
            Model& m = s_models[s.model];
            if (m.state == 3) { m.state = 0; FarTreesBake::RequestModel(m.name); }
            if (m.state != 1 || !s.buf || !s.buf->m_hasData) continue;
            m.usedMs = now;
            if (!begun) {
                g_theGxDevicePtr->RsPush();
                GxRsSet(GxRs_VertexShader, s_vs);
                GxRsSet(GxRs_PixelShader, s_ps);
                GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);   // cut-out by clip() in FarTreesPS
                GxRsSet(GxRs_AlphaRef, 0);
                GxRsSet(GxRs_DepthTest, 1);
                GxRsSet(GxRs_DepthFunc, 0);   // LESSEQUAL
                GxRsSet(GxRs_DepthWrite, 1);
                GxRsSet(GxRs_Culling, 0);
                GxRsSet(GxRs_Lighting, 0);
                GxRsSet(GxRs_Fog, 0);
                GxRsSet(GxRs_StencilTest, 0);
                GxRsSet(GxRs_ScissorTest, 0);
                GxRsSet(GxRs_ColorWrite, 7);
                FarLand::BindSunShadow();
                begun = true;
            }
            const FarTreesBake::ModelResult& r = m.data;
            const float F = static_cast<float>(FarTreesBake::kFrames);
            k[5][0] = r.centre[0]; k[5][1] = r.centre[1]; k[5][2] = r.centre[2]; k[5][3] = r.radius;
            k[6][0] = F; k[6][1] = F - 1.0f; k[6][2] = r.extent; k[6][3] = 1.0f / F;
            float fade = static_cast<float>(now - m.readyMs) / kFadeInMs;
            pc[3][0] = fade < 1.0f && now - m.readyMs < 0x80000000u ? fade : 1.0f;
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, &k[0][0], 10);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &pc[0][0], 4);
            GxRsSet(GxRs_Texture0, m.alb);
            GxRsSet(GxRs_Texture1, m.nrm);
            GxPrimVertexPtr(s.buf, GxVBF_PT);
            CGxBatch batch;
            batch.m_primType = GxPrim_Triangles;
            batch.m_start = 0;
            batch.m_count = s.count * 6;
            batch.m_minIndex = 0;
            batch.m_maxIndex = 0;
            g_theGxDevicePtr->Draw(&batch, 0);
        }
    }
    if (begun) { g_theGxDevicePtr->RsPop(); saved.Restore(); }
}

void Shutdown() {
    FarTreesBake::Stop();
}

bool ShadowWanted() {
    return s_enabled && s_shadow && !s_tiles.empty() && !s_failed && !s_shFailed && (CWorld::enables & Enable_Doodads) && (CWorld::enables & Enable_NoFullAlpha);
}

// Inside the band's RsPush, with its view (light look-at, camera-relative) and projection set. Every tree is tested
// in FarTreesShadowVS (fade hand-over, size class), so a tile costs one draw per model and nothing per tree here.
void DrawShadow(const CAaBox& extents, CFrustum& cull, const C3Vector& camera) {
    if (!g_theGxDevicePtr || !ShadowWanted() || !GpuShadow()) return;

    C44Matrix vp;
    GxXformViewProjNativeTranspose(vp);
    vp = vp.Transpose();
    const CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    const C44Matrix& V = stack.m_mtx[stack.m_level];
    const C3Vector& eye = CWorldScene::s_activeWorldView;
    const C3Vector& sun = CShadowCache::s_sunDir;   // light travel direction

    float k[11][4] = {};
    std::memcpy(k, &vp, sizeof(float) * 16);
    k[4][0] = camera.x; k[4][1] = camera.y; k[4][2] = camera.z;
    k[7][0] = CWorldScene::s_fadeDistMin[2]; k[7][1] = CWorldScene::s_fadeDistMin[3];
    k[8][0] = eye.x; k[8][1] = eye.y; k[8][2] = eye.z;
    k[9][0] = -sun.x; k[9][1] = -sun.y; k[9][2] = -sun.z;
    k[10][0] = V.a2; k[10][1] = V.b2; k[10][2] = V.c2; k[10][3] = V.d2;
    const float pc[4] = { 0.0f, 0.0f, 0.0f, CShadowCache::s_shadowScale };

    const float nearest = std::fmin(CWorldScene::s_fadeDistMin[2], CWorldScene::s_fadeDistMin[3]);
    const uint32_t now = World::s_m2Scene ? World::s_m2Scene->m_time : 0;
    bool begun = false;
    SavedConstants saved;
    for (auto& kv : s_tiles) {
        Tile& t = kv.second;
        if (!t.ready || t.places.empty()) continue;
        float far2 = 0.0f;
        const float c[3] = { eye.x, eye.y, eye.z };
        for (int i = 0; i < 3; ++i) {
            const float df = std::fmax(std::fabs(t.lo[i] - c[i]), std::fabs(t.hi[i] - c[i]));
            far2 += df * df;
        }
        if (far2 < nearest * nearest) continue;   // every tree still a real model, casting itself
        const CAaBox box(C3Vector(t.lo[0], t.lo[1], t.lo[2]), C3Vector(t.hi[0], t.hi[1], t.hi[2]));
        if (!extents.Intersects(box) || !cull.Cull(box) || !EnsureBuffers(t)) continue;

        for (const Segment& s : t.segs) {
            Model& m = s_models[s.model];
            if (m.state != 1 || !s.buf || !s.buf->m_hasData) continue;
            m.usedMs = now;
            if (!begun) {
                g_theGxDevicePtr->RsPush();
                GxRsSet(GxRs_VertexShader, s_shVs);
                GxRsSet(GxRs_PixelShader, s_shPs);
                GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);   // cut-out by clip() in FarTreesShadowPS
                GxRsSet(GxRs_AlphaRef, 0);
                GxRsSet(GxRs_DepthTest, 1);
                GxRsSet(GxRs_DepthFunc, 0);   // LESSEQUAL
                GxRsSet(GxRs_DepthWrite, 1);
                GxRsSet(GxRs_Culling, 0);
                GxRsSet(GxRs_Fog, 0);
                GxRsSet(GxRs_Texture1, 0);
                g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, pc, 1);
                begun = true;
            }
            const FarTreesBake::ModelResult& r = m.data;
            const float F = static_cast<float>(FarTreesBake::kFrames);
            k[5][0] = r.centre[0]; k[5][1] = r.centre[1]; k[5][2] = r.centre[2]; k[5][3] = r.radius;
            k[6][0] = F; k[6][1] = F - 1.0f; k[6][2] = r.extent; k[6][3] = 1.0f / F;
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, &k[0][0], 11);
            GxRsSet(GxRs_Texture0, m.alb);
            GxPrimVertexPtr(s.buf, GxVBF_PT);
            CGxBatch batch;
            batch.m_primType = GxPrim_Triangles;
            batch.m_start = 0;
            batch.m_count = s.count * 6;
            batch.m_minIndex = 0;
            batch.m_maxIndex = 0;
            g_theGxDevicePtr->Draw(&batch, 0);
        }
    }
    if (begun) { g_theGxDevicePtr->RsPop(); saved.Restore(); }
}

uint32_t CanopyGeneration() {
    return s_enabled && s_shadow ? s_generation : 0;
}

// Main thread, on TerrainSunShadow's snapshot (about 0.2 ms at 30k trees: no trig, the crown's offset from the trunk
// is dropped). Model space -> crown: the bake's centre is the middle of the drawn vertices' box and its radius that
// box's half diagonal, so with the origin at the trunk's foot the top is 2 cz and the crown's half width about
// sqrt(R^2 - cz^2) / sqrt(2) a side. ponytail: one round crown per tree; the box itself if the cache ever keeps it.
void Canopies(std::vector<Canopy>& out) {
    out.clear();
    if (!s_enabled || !s_shadow) return;
    for (auto& kv : s_tiles) {
        Tile& t = kv.second;
        if (!t.ready) continue;
        for (Place& p : t.places) {
            const Model& m = s_models[p.model];
            if (m.state != 1 && m.state != 3) continue;   // a dropped atlas keeps its shape
            const FarTreesBake::ModelResult& r = m.data;
            if (SizeClass(p) == 0.0f) continue;   // FarTreesVS: no impostor for these
            const float cz = std::fmax(r.centre[2], 0.0f);
            out.push_back({ p.x, p.y, p.z + 2.0f * cz * p.scale, std::sqrt(std::fmax(r.radius * r.radius - cz * cz, 0.0f)) * 0.7071f * p.scale });
        }
    }
}

} // namespace FarTrees
