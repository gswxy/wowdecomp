#include "WoW/Source/WorldClient/FarLand.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FarLandBake.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/World.h"

namespace FarLand {
namespace {

using FarLandBake::Tile;

// Window atlases (wxl-farfield's `albedo` layout): tile (row, col) lives in slot (row mod 32, col mod 32), so the
// 32 x 32 tiles round the camera fit; the horizon reaches at most ~12 tiles each way (farclip 1277 x 4).
constexpr uint32_t kWin = 32;
constexpr uint32_t kAtlasA = kWin * FarLandBake::kAlbedoPx;   // 4096, DXT1
constexpr uint32_t kAtlasN = kWin * FarLandBake::kNormalPx;   // 2048, DXT5
constexpr uint32_t kCacheVersion = 1;
constexpr float kFadeMs = 600.0f;

int32_t s_enabled = 0;
int32_t s_uploads = 4;

CGxShader* s_vs = nullptr;
CGxShader* s_ps = nullptr;
bool s_shaderFailed = false;

// ---------- atlases: a CPU copy of every level, latched by the texture callback ----------
struct Atlas {
    uint32_t px, blockBytes, levels;
    size_t offset[16];
    std::vector<uint8_t> chain;
    CTexture* tex = nullptr;

    void Init(uint32_t size, uint32_t bytes) {
        px = size;
        blockBytes = bytes;
        size_t o = 0;
        for (levels = 0; levels < 16 && (size >> levels); levels++) {
            offset[levels] = o;
            const uint32_t b = std::max((size >> levels) / 4, 1u);
            o += size_t(b) * b * bytes;
        }
        chain.assign(o, 0);
    }
    uint32_t BlocksPerRow(uint32_t l) const { return std::max((px >> l) / 4, 1u); }
    uint8_t* Block(uint32_t l, uint32_t bx, uint32_t by) { return &chain[offset[l] + (size_t(by) * BlocksPerRow(l) + bx) * blockBytes]; }
};
Atlas s_albedo, s_normal;

void AtlasUserFunc(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t mipLevel, void* userArg, uint32_t& stride, const void*& texels) {
    const Atlas* at = static_cast<const Atlas*>(userArg);
    if (command == GxTex_Latch && at && mipLevel < at->levels) {
        stride = at->BlocksPerRow(mipLevel) * at->blockBytes;
        texels = at->chain.data() + at->offset[mipLevel];
    }
}

void FlatNormals() {   // DXT5nm (0, 0, 1): alpha 128 / green 128 everywhere
    for (size_t i = 0; i < s_normal.chain.size(); i += 16) {
        uint8_t* b = &s_normal.chain[i];
        std::memset(b, 0, 16);
        b[0] = b[1] = 128;
        const uint16_t g = uint16_t(32 << 5);
        std::memcpy(b + 8, &g, 2);
        std::memcpy(b + 10, &g, 2);
    }
}

bool Atlases() {
    if (s_albedo.tex) return true;
    s_albedo.Init(kAtlasA, 8);
    s_normal.Init(kAtlasN, 16);
    FlatNormals();
    const CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);
    s_albedo.tex = TextureCreate(kAtlasA, kAtlasA, GxTex_Dxt1, GxTex_Dxt1, flags, &s_albedo, AtlasUserFunc, "FarLandAlbedo", 0);
    s_normal.tex = TextureCreate(kAtlasN, kAtlasN, GxTex_Dxt5, GxTex_Dxt5, flags, &s_normal, AtlasUserFunc, "FarLandNormal", 0);
    for (Atlas* at : { &s_albedo, &s_normal })
        if (CGxTex* g = at->tex ? TextureGetGxTex(at->tex, 0, nullptr) : nullptr) GxTexUpdate(g, 0, 0, at->px, at->px, 1);
    return s_albedo.tex && s_normal.tex;
}

// ---------- per map state (main thread) ----------
enum : uint8_t { kNone, kQueued, kReady, kFailed };
uint32_t s_loadedFor = 0xFFFFFFFFu;
uint8_t s_state[4096];
int16_t s_owner[kWin * kWin];
uint8_t s_slotMean[kWin * kWin][3];
float s_readyMs[4096];

float NowMs() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<float, std::milli>(steady_clock::now() - t0).count();
}

// ---------- worker ----------
struct Result {
    uint32_t gen;
    int32_t tile;
    std::unique_ptr<Tile> data;   // null = no ADT / bake failed
};
std::mutex s_lock;
std::condition_variable s_wake;
std::deque<int32_t> s_jobs;
std::vector<Result> s_done;
uint32_t s_gen = 0;
std::string s_map;
std::vector<uint8_t> s_liquidCategory;
std::atomic<int32_t> s_camTile { 0 };
bool s_workerStarted = false;

bool ReadClientFile(const std::string& path, std::vector<uint8_t>& out) {
    SFileImpl* f = nullptr;
    SFileOpenFile(path.c_str(), &f);
    if (!f) return false;
    out.resize(SFileGetFileSize(f, nullptr));
    const bool ok = out.empty() || SFileReadFile(f, out.data(), static_cast<uint32_t>(out.size()), nullptr, nullptr, 0);
    SFileCloseFile(f);
    return ok;
}

struct CacheHeader {
    char magic[4];
    uint32_t version, adtSize, adtCrc;
};

std::unique_ptr<Tile> BakeOrLoad(FarLandBake::Context& ctx, const std::string& map, int32_t t) {
    char name[320];
    std::snprintf(name, sizeof(name), "World\\Maps\\%s\\%s_%d_%d.adt", map.c_str(), map.c_str(), t % 64, t / 64);
    std::vector<uint8_t> adt;
    if (!ReadClientFile(name, adt) || adt.empty()) return nullptr;
    const CacheHeader want { { 'F', 'L', 'T', '1' }, kCacheVersion, uint32_t(adt.size()), FarLandBake::Crc32(adt.data(), adt.size()) };
    const std::filesystem::path dir = std::filesystem::path("Cache") / "FarLand" / map;
    char file[32];
    std::snprintf(file, sizeof(file), "%d_%d.flt", t % 64, t / 64);
    const std::filesystem::path path = dir / file;
    auto tile = std::make_unique<Tile>();
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {   // the cache: valid only for the very same ADT bytes
        CacheHeader h;
        const bool hit = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(&h, &want, sizeof(h)) == 0 && std::fread(tile.get(), sizeof(Tile), 1, f) == 1;
        std::fclose(f);
        if (hit) return tile;
    }
    if (!FarLandBake::Bake(ctx, adt.data(), adt.size(), *tile)) return nullptr;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        const bool ok = std::fwrite(&want, sizeof(want), 1, f) == 1 && std::fwrite(tile.get(), sizeof(Tile), 1, f) == 1;
        std::fclose(f);
        if (!ok) std::filesystem::remove(path, ec);   // never leave a torn entry behind
    }
    return tile;
}

void WorkerMain() {
    FarLandBake::Context ctx;
    ctx.read = ReadClientFile;
    uint32_t ctxGen = 0xFFFFFFFFu;
    std::string map;
    for (;;) {
        int32_t t;
        uint32_t gen;
        {
            std::unique_lock<std::mutex> lock(s_lock);
            s_wake.wait(lock, [] { return !s_jobs.empty(); });
            // nearest the camera's tile first
            const int32_t cam = s_camTile.load();
            auto dist = [cam](int32_t a) { const int32_t dr = a / 64 - cam / 64, dc = a % 64 - cam % 64; return dr * dr + dc * dc; };
            auto best = std::min_element(s_jobs.begin(), s_jobs.end(), [&](int32_t a, int32_t b) { return dist(a) < dist(b); });
            t = *best;
            s_jobs.erase(best);
            gen = s_gen;
            if (gen != ctxGen) {
                map = s_map;
                ctx.liquidCategory = s_liquidCategory;
            }
        }
        if (gen != ctxGen) {
            ctxGen = gen;
            std::vector<uint8_t> wdt;
            ctx.bigAlpha = ReadClientFile("World\\Maps\\" + map + "\\" + map + ".wdt", wdt) && FarLandBake::ReadBigAlpha(wdt);
        }
        std::unique_ptr<Tile> tile = BakeOrLoad(ctx, map, t);
        std::lock_guard<std::mutex> lock(s_lock);
        s_done.push_back({ gen, t, std::move(tile) });
    }
}

void NewMap() {
    s_loadedFor = g_lowDetailLoads;
    std::memset(s_state, kNone, sizeof(s_state));
    std::memset(s_owner, 0xFF, sizeof(s_owner));
    std::vector<uint8_t> cats;
    for (int32_t i = 0; i < g_liquidTypeDB.m_numRecords; i++) {
        const LiquidTypeRec& r = g_liquidTypeDB.m_records[i];
        if (r.m_id >= 0 && r.m_id < 4096) {
            if (cats.size() <= size_t(r.m_id)) cats.resize(r.m_id + 1, 0);
            cats[r.m_id] = uint8_t(std::min(std::max(r.m_soundBank, 0), 3));   // LiquidType type: water/ocean/magma/slime
        }
    }
    std::lock_guard<std::mutex> lock(s_lock);
    s_gen++;
    s_map = CMap::mapName;
    s_liquidCategory.swap(cats);
    s_jobs.clear();
    s_done.clear();
    if (!s_workerStarted) {
        s_workerStarted = true;
        std::thread(WorkerMain).detach();   // lives as long as the process; it sleeps on s_wake when idle
    }
}

void Request(int32_t t) {
    s_state[t] = kQueued;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_jobs.push_back(t);
    }
    s_wake.notify_one();
}

// Copy a finished tile into its slot: its own levels, then the shared levels where a block spans several slots
// (from the slot means), and upload the slot's rectangle.
void Upload(int32_t t, const Tile& tile) {
    const uint32_t row = t / 64, col = t % 64, sy = row % kWin, sx = col % kWin, slot = sy * kWin + sx;
    if (s_owner[slot] >= 0 && s_owner[slot] != t) s_state[s_owner[slot]] = kNone;   // evicted: asked again when seen
    s_owner[slot] = int16_t(t);
    std::memcpy(s_slotMean[slot], tile.mean, 3);
    for (uint32_t l = 0; l < FarLandBake::kAlbedoLevels; l++) {
        const uint32_t nb = (FarLandBake::kAlbedoPx >> l) / 4;
        for (uint32_t by = 0; by < nb; by++)
            std::memcpy(s_albedo.Block(l, sx * nb, sy * nb + by), tile.albedo + FarLandBake::AlbedoLevelOffset(l) + by * nb * 8, nb * 8);
    }
    for (uint32_t l = 0; l < FarLandBake::kNormalLevels; l++) {
        const uint32_t nb = (FarLandBake::kNormalPx >> l) / 4;
        for (uint32_t by = 0; by < nb; by++)
            std::memcpy(s_normal.Block(l, sx * nb, sy * nb + by), tile.normal + FarLandBake::NormalLevelOffset(l) + by * nb * 16, nb * 16);
    }
    for (uint32_t l = FarLandBake::kAlbedoLevels; l < s_albedo.levels; l++) {
        const uint32_t span = std::min(kWin, 1u << (l - FarLandBake::kAlbedoLevels + 1));   // slots a block covers, each way
        const uint32_t x0 = sx / span * span, y0 = sy / span * span;
        uint32_t sum[3] = {}, n = 0;
        for (uint32_t y = y0; y < y0 + span; y++)
            for (uint32_t x = x0; x < x0 + span; x++)
                if (s_owner[y * kWin + x] >= 0) {
                    for (int j = 0; j < 3; j++) sum[j] += s_slotMean[y * kWin + x][j];
                    n++;
                }
        const uint8_t rgb[3] = { uint8_t(sum[0] / n), uint8_t(sum[1] / n), uint8_t(sum[2] / n) };
        FarLandBake::SolidDxt1(rgb, s_albedo.Block(l, std::min(sx / span, s_albedo.BlocksPerRow(l) - 1), std::min(sy / span, s_albedo.BlocksPerRow(l) - 1)));
    }
    if (CGxTex* g = TextureGetGxTex(s_albedo.tex, 0, nullptr))
        GxTexUpdate(g, sx * FarLandBake::kAlbedoPx, sy * FarLandBake::kAlbedoPx, (sx + 1) * FarLandBake::kAlbedoPx, (sy + 1) * FarLandBake::kAlbedoPx, 1);
    if (CGxTex* g = TextureGetGxTex(s_normal.tex, 0, nullptr))
        GxTexUpdate(g, sx * FarLandBake::kNormalPx, sy * FarLandBake::kNormalPx, (sx + 1) * FarLandBake::kNormalPx, (sy + 1) * FarLandBake::kNormalPx, 1);
    s_state[t] = kReady;
    s_readyMs[t] = NowMs();
}

// Main thread, once a pass: take at most farLandUploads finished tiles.
void Drain() {
    std::vector<Result> take;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto end = s_done.begin() + std::min<size_t>(s_done.size(), size_t(std::max(s_uploads, 1)));
        std::move(s_done.begin(), end, std::back_inserter(take));
        s_done.erase(s_done.begin(), end);
    }
    for (Result& r : take) {
        if (r.gen != s_gen || s_state[r.tile] != kQueued) continue;
        if (!r.data) {
            s_state[r.tile] = kFailed;   // no ADT or an unreadable one: this tile stays stock
            continue;
        }
        Upload(r.tile, *r.data);
    }
}

bool Shaders() {
    if (s_shaderFailed) return false;
    if (!s_vs) {
        g_theGxDevicePtr->ShaderCreate(&s_vs, GxSh_Vertex, "Shaders\\Vertex", "FarLandVS", 1);
        g_theGxDevicePtr->ShaderCreate(&s_ps, GxSh_Pixel, "Shaders\\Pixel", "FarLandPS", 1);
    }
    if (!s_vs || !s_vs->Valid() || !s_ps || !s_ps->Valid()) {
        s_shaderFailed = true;   // no FarLand shaders in custom.MPQ: stock horizon
        ConsolePrintf("farLand: FarLandVS / FarLandPS missing from custom.MPQ");
        return false;
    }
    return true;
}

bool s_needConstants = true;   // set by BeginPass: once per RenderHorizon (the water reflection runs it again)

// Once a pass: view-projection (the horizon's, RenderHorizon has set it), camera, atlas mapping, sun and fog.
void PassConstants() {
    s_needConstants = false;
    C44Matrix vp;
    GxXformViewProjNativeTranspose(vp);
    vp = vp.Transpose();
    float vs[8][4] = {};
    std::memcpy(vs, &vp, sizeof(float) * 16);
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    vs[4][0] = cam.x; vs[4][1] = cam.y; vs[4][2] = cam.z;
    // uv = slot origin + 0.5 texel + t * (S - 1) texels: a tile edge lands on its edge texel's centre, so mip 0
    // never samples the next slot (a different tile).
    const float a = float(FarLandBake::kAlbedoPx - 1) / kAtlasA, n = float(FarLandBake::kNormalPx - 1) / kAtlasN;
    const float uv[2][4] = { { a, a, 0.5f / kAtlasA, 0.5f / kAtlasA }, { n, n, 0.5f / kAtlasN, 0.5f / kAtlasN } };
    std::memcpy(vs[6], uv, sizeof(uv));
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, &vs[0][0], 5);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 6, &vs[6][0], 2);

    // The terrain's sun (CMapRenderChunk::InitializeVertexShaderConstants): world direction towards the sun.
    float ps[5][4] = {};
    C3Vector sunDir(0.0f, 0.0f, 0.0f), amb(1.0f, 1.0f, 1.0f), dif(0.0f, 0.0f, 0.0f), spec;
    CM2Lighting lighting(sunDir);
    lighting.AddLight(&CMap::s_mapLight->m_light);
    if (lighting.GetSunLight(sunDir, amb, dif, spec)) {
        ps[0][0] = -sunDir.x; ps[0][1] = -sunDir.y; ps[0][2] = -sunDir.z;
    } else {
        amb = C3Vector(1.0f, 1.0f, 1.0f);
        dif = C3Vector(0.0f, 0.0f, 0.0f);
    }
    ps[1][0] = amb.x; ps[1][1] = amb.y; ps[1][2] = amb.z;
    ps[2][0] = dif.x; ps[2][1] = dif.y; ps[2][2] = dif.z;
    // The world's fog as the terrain gets it; vfog pushes it out of range, so its composite does the fogging.
    const DayNight::DNInfo& dn = DayNight::g_dnInfo;
    ps[3][0] = dn.m_fog.m_color.r / 255.0f; ps[3][1] = dn.m_fog.m_color.g / 255.0f; ps[3][2] = dn.m_fog.m_color.b / 255.0f;
    ps[4][0] = dn.m_fog.m_start;
    ps[4][1] = dn.m_fog.m_end > dn.m_fog.m_start + 0.01f ? 1.0f / (dn.m_fog.m_end - dn.m_fog.m_start) : 0.0f;
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &ps[0][0], 5);
}

} // namespace

void RegisterCVars() {
    CVar::Register("farLand", "Far land past the view distance in its real ground colour, lit by the sun (baked in the background, cached in Cache\\FarLand): 1 = on, 0 = stock fog-coloured horizon (default)", 1, "0", [](CVar*, const char*, const char* value, void*) -> bool {
        s_enabled = SStrToInt(value) != 0;
        return true;
    }, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("farLandUploads", "farLand: most baked tiles copied into the atlases per frame (1..32, default 4)", 1, "4", [](CVar*, const char*, const char* value, void*) -> bool {
        s_uploads = std::min(std::max(SStrToInt(value), 1), 32);
        return true;
    }, CATEGORY_GRAPHICS, false, nullptr, false);
}

void BeginPass() {
    s_needConstants = true;
    if (!s_enabled || !g_theGxDevicePtr) return;
    if (s_loadedFor != g_lowDetailLoads) NewMap();
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const int32_t row = std::min(std::max(int32_t((17066.666f - cam.x) / (1600.0f / 3.0f)), 0), 63);
    const int32_t col = std::min(std::max(int32_t((17066.666f - cam.y) / (1600.0f / 3.0f)), 0), 63);
    s_camTile.store(row * 64 + col);
    if (Atlases()) Drain();
}

bool Apply(const CMapAreaLow* area) {
    if (!s_enabled || !g_theGxDevicePtr || s_loadedFor != g_lowDetailLoads || !s_albedo.tex) return false;
    const int32_t t = (area->m_index.y & 63) * 64 + (area->m_index.x & 63);
    if (s_state[t] == kNone) Request(t);
    if (s_state[t] != kReady || !Shaders()) return false;
    CGxTex* albedo = TextureGetGxTex(s_albedo.tex, 0, nullptr);
    CGxTex* normal = TextureGetGxTex(s_normal.tex, 0, nullptr);
    if (!albedo || !normal) return false;

    // ponytail: constants re-sent per tile after the stock RsPush/RsPop; one RsPush for the whole pass if it ever shows in a profile.
    if (s_needConstants) PassConstants();
    GxRsSet(GxRs_VertexShader, s_vs);
    GxRsSet(GxRs_PixelShader, s_ps);
    GxRsSet(GxRs_Texture0, albedo);
    GxRsSet(GxRs_Texture1, normal);
    GxRsSet(GxRs_Fog, 0);
    const uint32_t row = t / 64, col = t % 64;
    const float tile[4] = { area->m_topLeft.x, area->m_topLeft.y, float(col % kWin) / kWin, float(row % kWin) / kWin };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 5, tile, 1);
    const float fade[4] = { std::min((NowMs() - s_readyMs[t]) / kFadeMs, 1.0f), 0.0f, 0.0f, 0.0f };   // cross-fade in from the stock fog colour
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 5, fade, 1);
    return true;
}

} // namespace FarLand
