#include "WoW/Source/WorldClient/FarLand.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/World.h"

namespace FarLand {
namespace {

int32_t s_enabled = 0;

CGxShader* s_vs = nullptr;
CGxShader* s_ps = nullptr;
bool s_shaderFailed = false;

// The loaded map's bake (FarLand\<map>.flnd + atlases), reloaded when CMapLowDetail loads another WDL.
uint32_t s_loadedFor = 0xFFFFFFFFu;
bool s_haveBake = false;
int16_t s_slot[4096];
uint32_t s_slotsX = 0, s_slotsY = 0;
float s_uvA[4], s_uvN[4];
CTexture* s_albedo = nullptr;
CTexture* s_normal = nullptr;

bool s_needConstants = true;   // set by BeginPass: once per RenderHorizon (the water reflection runs it again)

void FreeBake() {
    if (s_albedo) HandleClose(reinterpret_cast<HOBJECT>(s_albedo));
    if (s_normal) HandleClose(reinterpret_cast<HOBJECT>(s_normal));
    s_albedo = s_normal = nullptr;
    s_haveBake = false;
}

void LoadBake() {
    FreeBake();
    s_loadedFor = g_lowDetailLoads;
    char path[300];
    std::snprintf(path, sizeof(path), "FarLand\\%s.flnd", CMap::mapName);
    SFileImpl* file = nullptr;
    SFileOpenFile(path, &file);
    if (!file) return;   // no bake for this map: stock horizon
    struct Header { char magic[4]; uint32_t version; uint16_t slotsX, slotsY, slotA, slotN; } h;
    const uint32_t size = SFileGetFileSize(file, nullptr);
    bool ok = size == sizeof(Header) + sizeof(s_slot)
        && SFileReadFile(file, &h, sizeof(h), nullptr, nullptr, 0)
        && SFileReadFile(file, s_slot, sizeof(s_slot), nullptr, nullptr, 0)
        && std::memcmp(h.magic, "FLND", 4) == 0 && h.version == 1 && h.slotsX && h.slotsY && h.slotA > 1 && h.slotN > 1;
    SFileCloseFile(file);
    if (!ok) {
        ConsolePrintf("farLand: %s is not a version 1 bake", path);
        return;
    }
    s_slotsX = h.slotsX;
    s_slotsY = h.slotsY;
    // uv = slot origin + 0.5 texel + t * (S - 1) texels: a tile edge lands on its edge texel's centre, so the
    // bilinear tap never reaches the unrelated tile packed next to it (at mip 0).
    const float wa = float(h.slotsX * h.slotA), ha = float(h.slotsY * h.slotA);
    const float wn = float(h.slotsX * h.slotN), hn = float(h.slotsY * h.slotN);
    const float a[4] = { (h.slotA - 1) / wa, (h.slotA - 1) / ha, 0.5f / wa, 0.5f / ha };
    const float n[4] = { (h.slotN - 1) / wn, (h.slotN - 1) / hn, 0.5f / wn, 0.5f / hn };
    std::memcpy(s_uvA, a, sizeof(a));
    std::memcpy(s_uvN, n, sizeof(n));
    CGxTexFlags flags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1);
    CStatus status;
    std::snprintf(path, sizeof(path), "FarLand\\%s_a.blp", CMap::mapName);
    s_albedo = TextureCreate(path, flags, &status, 0);
    std::snprintf(path, sizeof(path), "FarLand\\%s_n.blp", CMap::mapName);
    s_normal = TextureCreate(path, flags, &status, 0);
    s_haveBake = s_albedo && s_normal;
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

// Once a pass: view-projection (the horizon's, RenderHorizon has set it), camera, atlas mapping, sun and fog.
void FrameConstants() {
    s_needConstants = false;
    C44Matrix vp;
    GxXformViewProjNativeTranspose(vp);
    vp = vp.Transpose();
    float vs[8][4] = {};
    std::memcpy(vs, &vp, sizeof(float) * 16);
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    vs[4][0] = cam.x; vs[4][1] = cam.y; vs[4][2] = cam.z;
    std::memcpy(vs[6], s_uvA, sizeof(s_uvA));
    std::memcpy(vs[7], s_uvN, sizeof(s_uvN));
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
    CVar::Register("farLand", "Far land past the view distance in its real ground colour, lit by the sun (baked FarLand\\<map> files): 1 = on, 0 = stock fog-coloured horizon (default)", 1, "0", [](CVar*, const char*, const char* value, void*) -> bool {
        s_enabled = SStrToInt(value) != 0;
        return true;
    }, CATEGORY_GRAPHICS, false, nullptr, false);
}

void BeginPass() {
    s_needConstants = true;
}

bool Apply(const CMapAreaLow* area) {
    if (!s_enabled || !g_theGxDevicePtr) return false;
    if (s_loadedFor != g_lowDetailLoads) LoadBake();
    if (!s_haveBake) return false;
    const int32_t slot = s_slot[(area->m_index.y & 63) * 64 + (area->m_index.x & 63)];
    if (slot < 0 || !Shaders()) return false;
    CGxTex* albedo = TextureGetGxTex(s_albedo, 0, nullptr);
    CGxTex* normal = TextureGetGxTex(s_normal, 0, nullptr);
    if (!albedo || !normal) return false;   // still loading: stock tile this frame

    // ponytail: constants re-sent per tile after the stock RsPush/RsPop; one RsPush for the whole pass if it ever shows in a profile.
    if (s_needConstants) FrameConstants();
    GxRsSet(GxRs_VertexShader, s_vs);
    GxRsSet(GxRs_PixelShader, s_ps);
    GxRsSet(GxRs_Texture0, albedo);
    GxRsSet(GxRs_Texture1, normal);
    GxRsSet(GxRs_Fog, 0);
    const float tile[4] = { area->m_topLeft.x, area->m_topLeft.y, float(slot % s_slotsX) / s_slotsX, float(slot / s_slotsX) / s_slotsY };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 5, tile, 1);
    return true;
}

} // namespace FarLand
