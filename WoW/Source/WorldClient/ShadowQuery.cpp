#include "ShadowQuery.h"
#include "WoW/Source/WorldClient/WorldConstants.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "WoW/Source/WorldClient/TerrainSunShadow.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/ShaderEffectManager.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/CFrustum.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "WoW/Source/WorldClient/FarTrees.h"

// Not stock: gfxSunShadows (wxl-light's Sun.cpp shadow_sun), gfxShadowSnap, gfxShadowAnchor (northlight).
static CVar* s_cvSunShadows;
static CVar* s_cvShadowSnap;
static CVar* s_cvShadowAnchor;
static CVar* s_cvShadowSlopeBias;
// Not stock (Badgermilk0 port): shadowMapSize, shadowSoftness, shadowCascadeReuse.
static CVar* s_cvShadowMapSize;
static CVar* s_cvShadowSoftness;
static CVar* s_cvShadowWaterStrength;
static CVar* s_cvShadowCascadeReuse;
static CVar* s_cvShadowTerrainCast;
static CVar* s_cvShadowUnitFarTrees;
// Not stock (shadow overhaul step 1): shadowCasterMinTexels.
static CVar* s_cvShadowCasterMinTexels;
static CVar* s_cvShadowBandDoodads;
static CVar* s_cvShadowAlphaMatch;
// Not stock (shadow overhaul step 2): shadowWmoInterior.
static CVar* s_cvShadowWmoInterior;

#if defined(_WIN64)
static const char* const kShadowMapSizeDefault = "4096";   // ~600 MB of maps at extShadowQuality 3-4
#else
static const char* const kShadowMapSizeDefault = "0";      // Wow32: a 32-bit address space keeps the stock size
#endif

/// shadowMapSize: 0 = stock, else the power of two from 1024 to 8192 at or above the value.
static uint32_t ShadowMapSize() {
    int32_t value = s_cvShadowMapSize ? s_cvShadowMapSize->m_intValue : 0;

    if (value <= 0) {
        return 0;
    }

    uint32_t size = 1024;

    while (size < static_cast<uint32_t>(value) && size < 8192) {
        size *= 2;
    }

    return size;
}

/// shadowSoftness: the PCF kernel scale, 0.25 to 4.
static float ShadowSoftness() {
    float value = s_cvShadowSoftness ? s_cvShadowSoftness->m_floatValue : 1.0f;
    return value < 0.25f ? 0.25f : (value > 4.0f ? 4.0f : value);
}

/// shadowWaterStrength: how much of a sun shadow water shows, 0 to 1 (not stock).
float CShadowQuery::WaterShadowStrength() {
    float value = s_cvShadowWaterStrength ? s_cvShadowWaterStrength->m_floatValue : 0.25f;
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// STATIC: 0x00D25300
uint8_t CShadowQuery::s_stamp;

// STATIC: 0x00D2538C
static uint32_t s_maxTexturedBatches;

// STATIC: 0x00D25390
static uint32_t s_maxUntexturedBatches;

// STATIC: 0x00D25304
float CShadowQuery::s_cutoffHeight;

// STATIC: 0x00D25320
ShadowQueryBuffers CShadowQuery::s_bandBuffers[3];

static int32_t ConsoleCommand_ShadowSunInfo(const char*, const char*);   // shadowSunInfo, after SunShadowDir's statics

// VERIFY: a0192ad6 auto
// OFFSET: 0x007BD3A0
void CShadowQuery::Initialize() {
    CShadowCache::SetMatrixCallback(CShadowQuery::MatrixCallback);
    CShadowCache::SetFrustumCallback(CShadowQuery::FrustumCallback);
    CShadowCache::SetQueryCallback(CShadowQuery::QueryCallback);
    CShadowCache::SetRenderCallback(CShadowQuery::RenderCallback);

    g_shadowCull = CVar::Register("shadowCull", "enable shadow frustum culling", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    g_shadowScissor = CVar::Register("shadowScissor", "enable scissoring when rendering shadowmaps", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    g_shadowInstancing = CVar::Register("shadowInstancing", "enable instancing when rendering shadowmaps", 0, "1", nullptr, CATEGORY_DEFAULT, false, nullptr, false);
    ConsoleCommandRegister("shadowSunInfo", ConsoleCommand_ShadowSunInfo, CATEGORY_GRAPHICS, "Prints the shadow light's elevation and bearing beside the lighting's and the sun and moon discs'");
    s_cvSunShadows = CVar::Register("gfxSunShadows", "Shadows fall from the sun or moon in the sky (0 = stock steep light)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowSnap = CVar::Register("gfxShadowSnap", "Shadow maps move in whole texels so shadow edges do not crawl (0 = stock)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowAnchor = CVar::Register("gfxShadowAnchor", "Shadow cascades stay put while the player jumps (0 = stock)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowSlopeBias = CVar::Register("gfxShadowSlopeBias", "Extra shadow depth bias at a low sun, in band texels times cot(elevation) up to 3 (0 = stock)", 1, "1.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowMapSize = CVar::Register("shadowMapSize", "Shadow map size, a power of two from 1024 to 8192 (0 = stock 1024/2048; PCF filtering or Vulkan only)", 1, kShadowMapSizeDefault, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowSoftness = CVar::Register("shadowSoftness", "Scales the shadow PCF filter, 0.25 to 4 (1 = stock)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowWaterStrength = CVar::Register("shadowWaterStrength", "How strong sun shadows are on water, 0 to 1 (1 = as strong as on land): the surface's sun light and glint and the lake bed seen through it", 1, "0.25", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowTerrainCast = CVar::Register("shadowTerrainCast", "ADT terrain casts into the sun shadow maps (0 = stock: models and WMOs only)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowUnitFarTrees = CVar::Register("shadowUnitFarTrees", "Far tree impostors also cast into the 20 yd unit map (0 = off, default: the bands carry them; at a low sun the unit map's square runs 40 / sin(elevation) yd along the sun with the finest bias, recentred on the camera every frame)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowCasterMinTexels = CVar::Register("shadowCasterMinTexels", "Smallest caster in the 320/1280 yd shadow bands, in band texels of radius (0 = stock 2/10 yd)", 1, "6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowBandDoodads = CVar::Register("shadowBandDoodads", "extShadowQuality 3-4: billboard doodads and interior-flagged WMOs cast into the far shadow bands (0 = stock: the 20 yd unit map only)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowAlphaMatch = CVar::Register("shadowAlphaMatch", "Leaf and alpha-blended surfaces cast exactly where they draw (0 = stock: M2 cut at half alpha, WMO glass and glow cast solid)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowWmoInterior = CVar::Register("shadowWmoInterior", "Interior-flagged WMO groups (walls, roofs, towers seen from outside) cast the sun shadow, and WMO walls cast before their textures load (0 = stock: exterior groups only, loaded textures only)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvShadowCascadeReuse = CVar::Register("shadowCascadeReuse", "extShadowQuality 5: redraw the 320 yd cascade every 2nd frame and the 1280 yd one every 4th (0 = stock)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // (before CShadowCache::Initialize creates the maps, so a saved size needs no second Init)
    CShadowCache::s_sizeOverride = ShadowMapSize();
    CShadowCache::s_pcfScale = ShadowSoftness();

    CShadowCache::Initialize();
}

// OFFSET: 0x007BA7D0
void CShadowQuery::FreeBuffers() {
    for (ShadowQueryBuffers* buffers = CShadowQuery::s_bandBuffers; buffers < CShadowQuery::s_bandBuffers + sizeof(CShadowQuery::s_bandBuffers) / sizeof(CShadowQuery::s_bandBuffers[0]); buffers++) {
        if (buffers->m_groups) {
            SMemFree(buffers->m_groups, nullptr, 0, 0);
        }

        if (buffers->m_groupData) {
            SMemFree(buffers->m_groupData, nullptr, 0, 0);
        }

        delete[] buffers->m_untextured.m_batches;
        delete[] buffers->m_textured.m_batches;
    }
}

// VERIFY: 11a5d7a4 manual
// OFFSET: 0x007BB3E0
static void GetViewerPosition(C3Vector* position) {
    *position = DayNight::GetInfo()->m_cameraPos;
    CGCamera* camera = CGWorldFrame::GetActiveCamera();

    if (!camera) {
        return;
    }

    CGObject_C* object = ClntObjMgrObjectPtr(camera->m_targetGUID, TYPE_OBJECT, __FILE__, __LINE__);

    if (object) {
        *position = object->GetPosition();
    }
}

// The shadow light (gfxSunShadows): the sun or moon disc DayNight chose (g_lightBody), kept between these elevations.
// Under the floor shadows run off to infinity and the maps' fixed bias lets go of the ground; over the ceiling
// noon is flat. (Re-applied from e258860; gfxSunArc, in DayNight, is its arc. Without it the stock sun and moon share one bearing, 45 deg,
// the same as the stock world light's, so the shading and the shadows agree on the side and only the length
// follows the clock.)
// The floor was 12 deg until 2026-10-24: from 18:36 to sunset (gfxSunArc) the cascades and the land shadow then held
// the sun at twice its height, so a mesa's shadow fell short of a valley floor the sky showed in its shade while a
// tree's (just as short, but on the floor) lay across it (Durotar video, 2026-10-24). 4 deg: shadows 14x a caster's
// height, inside band 2. Terrain casts only its back faces and the M2/WMO receivers lift |N.L| < 0.2, so the
// lit ground under a grazing sun has nothing to acne against.
static const float kShadowMinElev = 4.0f, kShadowMaxElev = 70.0f;   // degrees
// Degrees the light may move before the cached maps are redrawn, at 30 degrees up; scaled by sin^2 so a low sun
// steps finer, never under kLatchFloor. (At 0.25 each step was a visible shift of every shadow, the user,
// 2026-10-06; at 0.01 the light steps every two or three seconds by a twenty-fifth of that.)
static const float kLatchStep = 0.01f, kLatchFloor = 0.004f;
static const float kLatchJump = 5.0f;          // degrees: more than this at once (sun to moon) is taken at once
static const uint32_t kLatchWaitMs = 2000;     // a step waits for the near maps to finish, but not longer
static C3Vector s_latch(0.0f, 0.0f, 1.0f);     // toward the light, as the shadow maps have it
static int32_t s_haveLatch;
static uint32_t s_latchMs;
static bool s_indoors;              // the camera's interior blend, with hysteresis (SunShadowDir, CameraIndoors)
static uint32_t s_latchSeenMs;     // the last frame SunShadowDir took the latch (SunLatched)

/// Toward the sun or moon (unit) and its height in degrees, as DayNight::SetPlanets placed the discs. (Kept from
/// SetPlanets' own sines rather than disc minus camera: at world coordinates that difference is noisy enough, a few
/// thousandths of a degree, to trip the latch every frame while moving.)
static bool LightBody(C3Vector& toward, float& elevDeg) {
    const C3Vector dir[2] = { DayNight::g_sunToward, DayNight::g_moonToward };
    float elev[2];

    for (int32_t b = 0; b < 2; b++) {
        if (!std::isfinite(dir[b].z)) {
            return false;
        }

        float z = dir[b].z < -1.0f ? -1.0f : (dir[b].z > 1.0f ? 1.0f : dir[b].z);
        elev[b] = std::asin(z) * 57.29578f;
    }

    // The body DayNight made the light (gfxSunArc handover), so the shadows change sides on the frame it does.
    const int32_t body = DayNight::g_lightBody == 1 ? 1 : 0;
    toward = dir[body];
    elevDeg = elev[body];
    return true;
}

// shadowSunInfo: elevation / compass bearing (0 north, 90 east; x is north, y west) of the directions toward the light.
static void PrintToward(const char* what, float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-6f)) {
        ConsolePrintf("  %-28s none", what);
        return;
    }
    float az = std::atan2(-y, x) * 57.29578f;
    az = az < 0.0f ? az + 360.0f : az;
    ConsolePrintf("  %-28s elev %6.2f  az %6.2f", what, std::asin(std::clamp(z / len, -1.0f, 1.0f)) * 57.29578f, az);
}

static int32_t ConsoleCommand_ShadowSunInfo(const char*, const char*) {
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    const float hours = info.m_dayProgression * 24.0f;
    ConsolePrintf("shadowSunInfo: game time %02d:%02d, light body %s, gfxSunArc %d", static_cast<int>(hours), static_cast<int>((hours - std::floor(hours)) * 60.0f),
                  DayNight::g_lightBody == 1 ? "moon" : "sun", DayNight::SunArc() ? 1 : 0);
    PrintToward("sun disc (SetPlanets)", DayNight::g_sunToward.x, DayNight::g_sunToward.y, DayNight::g_sunToward.z);
    PrintToward("moon disc", DayNight::g_moonToward.x, DayNight::g_moonToward.y, DayNight::g_moonToward.z);
    const C3Vector& l = info.m_light1.m_dir;
    PrintToward("lighting (m_light1, shading)", -l.x, -l.y, -l.z);
    const C3Vector& d = CShadowCache::s_sunDir;
    PrintToward("shadow maps (s_sunDir)", -d.x, -d.y, -d.z);
    if (s_haveLatch) {
        PrintToward("shadow latch (gfxSunShadows)", s_latch.x, s_latch.y, s_latch.z);
    } else {
        ConsolePrintf("  shadow latch: none (stock light: gfxSunShadows 0 or indoors)");
    }
    const float* land = TerrainSunShadow::LightDir();
    PrintToward("land term (shadowMask)", land[0], land[1], land[2]);
    ConsolePrintf("  shadow elevation kept within %.0f-%.0f deg; shading capped at the stock light's height (gfxSunArcMatch)", kShadowMinElev, kShadowMaxElev);
    return 1;
}

/// The direction the shadow maps' light travels this frame; false = the stock light.
///
/// Cached maps (shadow quality 3..4) are drawn for one direction and projected with whatever is stored now, so the
/// stored direction moves in steps, and each step sends the idle maps round again (CShadowCache::s_bandRedraw).
static bool SunShadowDir(C3Vector& out) {
    EShadowMode mode = CShadowCache::GetShadowMode();
    bool cached = mode > SHADOW_MEDIUM && mode < SHADOW_VERY_HIGH;

    // Camera in a WMO interior: the stock steep light, so a low sun does not throw unit shadows across the interior
    // dynamic map onto the walls (Ratchet inn, AUDIT-2026-10-23 bug 1). Hysteresis so a doorway does not flap.
    float blend = VolumetricFog::CameraInteriorBlend();
    s_indoors = s_indoors ? blend > 0.4f : blend >= 0.6f;

    if (!s_cvSunShadows || !s_cvSunShadows->m_intValue || s_indoors) {
        if (s_haveLatch && cached) {
            CShadowCache::s_bandRedraw |= 7u;   // back to the stock light: the cached maps are for the old one
        }

        s_haveLatch = 0;
        return false;
    }

    C3Vector body;
    float elev;

    if (!LightBody(body, elev)) {
        return false;
    }

    // Toward the body, its elevation kept inside the floor and ceiling, its bearing as it is.
    float clamped = elev < kShadowMinElev ? kShadowMinElev : (elev > kShadowMaxElev ? kShadowMaxElev : elev);
    float el = clamped * 0.017453292f;
    float h = std::sqrt(body.x * body.x + body.y * body.y);
    C3Vector want(0.0f, 0.0f, 1.0f);

    if (h > 1e-4f) {
        want = C3Vector(body.x / h * std::cos(el), body.y / h * std::cos(el), std::sin(el));
    }

    uint32_t now = OsGetAsyncTimeMs();
    bool step = !s_haveLatch;   // (quality 5 latches too: a moving light slides the snapped texel grid every frame)
    bool jumped = false;
    const CShadowBand* bands = CShadowCache::s_extShadowBands;

    if (!step) {
        // By the chord, not acos of the dot: a hundredth of a degree is under what a float's dot can tell from 1.
        float dx = want.x - s_latch.x, dy = want.y - s_latch.y, dz = want.z - s_latch.z;
        float moved = std::sqrt(dx * dx + dy * dy + dz * dz) * 57.29578f;
        float s = want.z * want.z / 0.25f;   // sin^2 against 30 degrees
        s = s < 0.1f ? 0.1f : (s > 1.0f ? 1.0f : s);
        float threshold = kLatchStep * s > kLatchFloor ? kLatchStep * s : kLatchFloor;
        bool idle = !cached || (bands[0].m_updateCount == 0 && bands[1].m_updateCount == 0);
        jumped = moved > kLatchJump;
        step = jumped || (moved > threshold && (idle || now - s_latchMs > kLatchWaitMs));
    }

    if (step) {
        bool first = !s_haveLatch;
        s_latch = want;
        s_haveLatch = 1;
        s_latchMs = now;

        if (cached) {
            for (uint32_t c = 0; c < 3; c++) {
                // (after a jump, one caught half redrawn too: its finished tiles are for the old direction)
                if (bands[c].m_updateCount == 0 || first || jumped) {
                    CShadowCache::s_bandRedraw |= 1u << c;
                }
            }
        }
    }

    s_latchSeenMs = now;
    out = C3Vector(-s_latch.x, -s_latch.y, -s_latch.z);
    return true;
}

// Toward the light the cascades were drawn for, while gfxSunShadows latches it. "Recently" rather than "this frame":
// the one LOWEST frame of a map recreate must not flip the land shadow (TerrainSunShadow) to the true sun and back.
bool CShadowQuery::CameraIndoors() {
    return s_indoors;
}

bool CShadowQuery::SunLatched(C3Vector& toward) {
    if (!s_haveLatch || OsGetAsyncTimeMs() - s_latchSeenMs > 1000u) {
        return false;
    }

    toward = s_latch;
    return true;
}

// Jump-stable cascade anchor (gfxShadowAnchor), after northlight's src/shadows/cascade_anchor.h (MIT, (c) 2026
// lepola): the bands follow the viewer's x/y, but their z is the lowest viewer z over the last second, so a jump
// (up and down in under a second) leaves the bands where they were and sends no cached band round again. Going
// down follows at once; climbing lags by at most a second of climb, never more than kAnchorMaxLag. A teleport or
// a new map starts afresh.
static const uint32_t kAnchorWindowMs = 1000;
static const float kAnchorReset = 40.0f;      // yd moved in one frame that counts as a teleport
static const float kAnchorMaxLag = 24.0f;     // yd; band 0 reaches 40 yd, so the player stays well inside it
static const uint32_t kAnchorCapacity = 256;  // window entries; when full the oldest drops (a shorter window)

struct AnchorSample {
    uint32_t ms;
    float z;
};

static AnchorSample s_anchorMinima[kAnchorCapacity];   // ring, z and time increasing from the front
static uint32_t s_anchorHead, s_anchorCount;
static C3Vector s_anchorLast;
static int32_t s_anchorMap = -1;

static C3Vector JumpStableCentre(const C3Vector& pivot, uint32_t now) {
    float dx = pivot.x - s_anchorLast.x, dy = pivot.y - s_anchorLast.y, dz = pivot.z - s_anchorLast.z;

    if (s_anchorMap != DayNight::s_mapId || dx * dx + dy * dy + dz * dz > kAnchorReset * kAnchorReset) {
        s_anchorCount = 0;
    }

    s_anchorMap = DayNight::s_mapId;
    s_anchorLast = pivot;

    while (s_anchorCount && s_anchorMinima[(s_anchorHead + s_anchorCount - 1) % kAnchorCapacity].z >= pivot.z) {
        s_anchorCount--;
    }

    if (s_anchorCount == kAnchorCapacity) {
        s_anchorHead = (s_anchorHead + 1) % kAnchorCapacity;
        s_anchorCount--;
    }

    s_anchorMinima[(s_anchorHead + s_anchorCount) % kAnchorCapacity] = { now, pivot.z };
    s_anchorCount++;

    while (now - s_anchorMinima[s_anchorHead].ms > kAnchorWindowMs) {   // the newest sample always stays
        s_anchorHead = (s_anchorHead + 1) % kAnchorCapacity;
        s_anchorCount--;
    }

    float lowest = pivot.z - kAnchorMaxLag;
    float z = s_anchorMinima[s_anchorHead].z;
    return C3Vector(pivot.x, pivot.y, z > lowest ? z : lowest);
}

// VERIFY: aa0465cc auto
// OFFSET: 0x007BB570
void CShadowQuery::Update() {
    C3Vector direction = CMap::s_mapLight->m_light.m_dir;
    int32_t useCullFrustum = CWorldScene::frustumPortalView.m_maxViewDepth >= 0.0f || CWorldScene::s_hasPendingExteriorGroups;

    // gfxSunShadows: the true sun (or moon) direction instead of the world light steepened (z x5, clamped).
    if (!SunShadowDir(direction)) {
        direction = CMap::s_mapLight->m_light.m_dir;
        float z = direction.z * 5.0f;

        if (z < -1.2f) {
            z = -1.2f;
        }

        float inv = 1.0f / std::sqrt(direction.y * direction.y + direction.x * direction.x + z * z);
        direction.x = direction.x * inv;
        direction.y = direction.y * inv;
        direction.z = z * inv;
    }

    CShadowCache::PreUpdate(direction);
    C3Vector viewer(0.0f, 0.0f, 0.0f);
    GetViewerPosition(&viewer);

    CShadowCache::s_texelSnap = s_cvShadowSnap && s_cvShadowSnap->m_intValue;
    CShadowCache::s_bandDoodads = s_cvShadowBandDoodads && s_cvShadowBandDoodads->m_intValue;
    CShadowCache::s_alphaMatch = s_cvShadowAlphaMatch && s_cvShadowAlphaMatch->m_intValue;
    CShadowCache::s_wmoInterior = s_cvShadowWmoInterior && s_cvShadowWmoInterior->m_intValue;

    // (a change of caster set redraws the cached bands)
    static float s_lastCasterKey = -1.0f;
    const float casterKey = CShadowCache::s_bandDoodads + 2.0f * CShadowCache::s_alphaMatch + 4.0f * CShadowCache::s_wmoInterior + 8.0f * (s_cvShadowCasterMinTexels ? s_cvShadowCasterMinTexels->m_floatValue : 0.0f);

    if (casterKey != s_lastCasterKey) {
        s_lastCasterKey = casterKey;
        CShadowCache::s_bandRedraw |= 7u;
    }
    CShadowCache::s_cascadeReuse = s_cvShadowCascadeReuse && s_cvShadowCascadeReuse->m_intValue;

    // shadowMapSize / shadowSoftness, live: CShadowCache::Update rebuilds the maps at a new size this frame.
    uint32_t mapSize = ShadowMapSize();

    if (mapSize != CShadowCache::s_sizeOverride) {
        CShadowCache::s_sizeOverride = mapSize;
        CShadowCache::s_recreateTextures = 1;
    }

    float softness = ShadowSoftness();

    if (softness != CShadowCache::s_pcfScale) {
        CShadowCache::s_pcfScale = softness;

        if (CShadowCache::s_shadowSize) {
            CShadowCache::InitPcfOffsets();
        }
    }
    CShadowCache::s_haveBandCentre = s_cvShadowAnchor && s_cvShadowAnchor->m_intValue;

    if (CShadowCache::s_haveBandCentre) {
        CShadowCache::s_bandCentre = JumpStableCentre(viewer, OsGetAsyncTimeMs());
    } else {
        s_anchorCount = 0;
    }
    CShadowCache::SetDebugParams(1.0f, 0);
    CShadowCache::Update(viewer, useCullFrustum);
    CShadowCache::PostUpdate();
}

// VERIFY: 7421bb67 manual
// OFFSET: 0x007BAC10
void CShadowQuery::MatrixCallback(C3Vector& center, float radius, C44Matrix& matrix, C3Vector& direction, int32_t band) {
    if (radius <= 0.0f) {
        return;
    }

    C44Matrix ortho;
    C44Matrix view;
    C44Matrix inverseView;
    C44Matrix lookAt;
    C3Vector eye(0.0f, 0.0f, 0.0f);
    C3Vector target(0.0f, 0.0f, 0.0f);
    C3Vector camera(0.0f, 0.0f, 0.0f);
    CGWorldFrame::GetCameraPosition(&camera);

    eye.x = center.x - CShadowCache::s_sunDir.x * 2000.0f - camera.x;
    eye.y = center.y - CShadowCache::s_sunDir.y * 2000.0f - camera.y;
    eye.z = center.z - 2000.0f * CShadowCache::s_sunDir.z - camera.z;
    target.x = center.x - camera.x;
    target.y = center.y - camera.y;
    target.z = center.z - camera.z;

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = stack.m_mtx[stack.m_level];
    C44Matrix copy = view;
    float determinant = copy.Determinant();
    view = copy.Inverse(determinant);
    inverseView = view;

    GxuXformCreateOrtho(-radius, radius, -radius, radius, 1.0f, 4000.0f, ortho);
    GxuXformCreateLookAtXXX(eye, target, direction, lookAt);
    matrix = inverseView * lookAt * ortho;

    C44Matrix depth = inverseView * lookAt;
    matrix.a2 = depth.a2;
    matrix.b2 = depth.b2;
    matrix.c2 = depth.c2;
    matrix.d2 = depth.d2;

    float bias = CShaderEffect::s_usePcfFiltering ? 0.5f : 0.0f;
    float scale;

    // Not stock (gfxShadowSlopeBias): the stock bias was tuned for a steep light. A sun-facing surface changes
    // light depth by texel * cot(elevation) per texel and PCF looks ~1.5 texels away, so a low sun needs more in
    // the wide bands. ponytail: constant slope term per band; a receiver normal offset is the proper fix.
    if (band >= 0 && s_cvShadowSlopeBias && CShadowCache::s_textureSize[0] > 0.0f) {
        const C3Vector& d = CShadowCache::s_sunDir;
        float k = std::isfinite(s_cvShadowSlopeBias->m_floatValue) ? std::clamp(s_cvShadowSlopeBias->m_floatValue, 0.0f, 4.0f) : 0.0f;
        float h = std::sqrt(d.x * d.x + d.y * d.y);
        float cot = std::fabs(d.z) > 1e-4f ? std::min(h / std::fabs(d.z), 3.0f) : 3.0f;
        bias += k * (2.0f * radius / CShadowCache::s_textureSize[0]) * cot;
    }

    if (band == 0) {
        scale = 2.0f;
    } else if (band == 1) {
        matrix.d2 = depth.d2 - (bias + 4.0f * 0.2f);
        return;
    } else if (band == 2) {
        matrix.d2 = depth.d2 - (bias + 8.0f * 0.2f);
        return;
    } else {
        scale = 0.5f;
    }

    matrix.d2 = depth.d2 - (bias + scale * 0.2f);
}

// VERIFY: de87a831 manual
// OFFSET: 0x007BAFD0
void CShadowQuery::FrustumCallback(CFrustum& frustum, C44Matrix& matrix, const C3Vector& direction, CShadowCacheQueryData& data, int32_t band) {
    C3Vector& pos = data.m_pos[band];
    float* bounds = data.m_bounds[band];
    C3Vector eye;
    eye.x = pos.x - CShadowCache::s_sunDir.x * 2000.0f;
    eye.y = pos.y - CShadowCache::s_sunDir.y * 2000.0f;
    eye.z = pos.z - 2000.0f * CShadowCache::s_sunDir.z;

    GxuXformCreateOrtho(bounds[0], bounds[1], bounds[2], bounds[3], 1.0f, 4000.0f, matrix);

    C44Matrix view;
    C3Vector corners[8];

    for (uint32_t i = 0; i < 8; i++) {
        corners[i] = C3Vector(0.0f, 0.0f, 0.0f);
    }

    GxuXformCreateLookAtXXX(eye, pos, direction, view);
    GxuXformCalcFrustumCorners(view, matrix, corners);
    frustum.CalcPlanesFromCorners(corners);
    frustum.InvertPlanes();
    frustum.SwapCorners();

    float* rect = data.m_cullRects[band];
    rect[0] = -1.0f;
    rect[1] = -1.0f;
    rect[2] = 1.0f;
    rect[3] = 1.0f;

    CFrustum& cull = data.m_bandFrustums[band];
    cull = frustum;

    if (data.m_queryFlags[band] == 8 || !g_shadowCull->m_intValue) {
        return;
    }

    CFrustum clip = CWorldScene::s_clipFrustum;
    clip.Transform(view * matrix);
    CAaBox box = CAaBox::Bounding(clip.corners, 8);

    // Not stock (shadowCascadeReuse): a band that may be kept a few frames covers 5% of its width more on each side,
    // what about 2 degrees of turn uncovers at its far edge (CShadowCache's CascadeDue redraws past that).
    if (CShadowCache::s_cascadeReuse && band > 0) {
        box.b.x -= 0.1f;
        box.b.y -= 0.1f;
        box.t.x += 0.1f;
        box.t.y += 0.1f;
    }

    box.b = C3Vector::Max(box.b, C3Vector(-1.0f, -1.0f, -1.0f));
    box.b = C3Vector::Min(box.b, C3Vector(1.0f, 1.0f, 1.0f));
    box.t = C3Vector::Max(box.t, C3Vector(-1.0f, -1.0f, -1.0f));
    box.t = C3Vector::Min(box.t, C3Vector(1.0f, 1.0f, 1.0f));

    if (box.t.x > box.b.x && box.t.y > box.b.y && box.t.z > box.b.z) {
        C44Matrix ortho;
        GxuXformCreateOrtho(-(bounds[0] * box.b.x), bounds[1] * box.t.x, -(bounds[2] * box.b.y), box.t.y * bounds[3], 1.0f, 4000.0f, ortho);
        GxuXformCalcFrustumCorners(view, ortho, corners);
        cull.CalcPlanesFromCorners(corners);
        cull.InvertPlanes();
        cull.SwapCorners();
        rect[1] = box.b.x;
        rect[3] = box.t.x;
        rect[0] = -box.t.y;
        rect[2] = -box.b.y;
    }
}

// Per band caps (stock 0x800 groups, 0x2000 M2 batches; past them casters drop out at random). Raised with
// shadowWmoInterior adding interior groups: a q5 band 2 (640 yd) over a city holds thousands. About 1 MB a band.
static const uint32_t kShadowGroupCap = 0x2000, kShadowBatchCap = 0x8000;

// VERIFY: dc5e2519 manual
// OFFSET: 0x007BA840
void CShadowQuery::AllocBuffers() {
    for (uint32_t i = 0; i < 3; i++) {
        ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[i];
        buffers.m_groupCount = 0;
        buffers.m_untextured.m_count = 0;
        buffers.m_untextured.m_maxCount = kShadowBatchCap;
        buffers.m_textured.m_count = 0;
        buffers.m_textured.m_maxCount = kShadowBatchCap;

        if (!buffers.m_groups) {
            buffers.m_groups = SMemAlloc(kShadowGroupCap * sizeof(CMapObjGroup*), __FILE__, __LINE__, 0);
        }

        if (!buffers.m_groupData) {
            buffers.m_groupData = SMemAlloc(kShadowGroupCap * sizeof(C44Matrix*), __FILE__, __LINE__, 0);
        }

        if (!buffers.m_untextured.m_batches) {
            buffers.m_untextured.m_batches = static_cast<CModelBatch*>(SMemAlloc(sizeof(CModelBatch) * kShadowBatchCap, __FILE__, __LINE__, 0));
        }

        if (!buffers.m_textured.m_batches) {
            buffers.m_textured.m_batches = static_cast<CModelBatch*>(SMemAlloc(sizeof(CModelBatch) * kShadowBatchCap, __FILE__, __LINE__, 0));
        }
    }
}

// VERIFY: 3e3dfd04 manual
// OFFSET: 0x007BD200
int32_t CShadowQuery::QueryCallback(CShadowCacheQueryData& data, uint32_t frame) {
    CShadowQuery::AllocBuffers();

    if (!++CShadowQuery::s_stamp) {
        CShadowQuery::s_stamp = 1;
    }

    int32_t flags = 0;

    for (int32_t i = 0; i <= data.m_lastBand; i++) {
        flags |= data.m_queryFlags[i];
    }

    CShadowQuery::GatherShadowCasters(data, 0, data.m_lastBand);

    if (data.m_useFrustum) {
        if (flags & 0xC) {
            CFrustum& frustum = data.m_frustums[data.m_lastBand];
            C3Vector center(0.0f, 0.0f, 0.0f);
            center.x = frustum.corners[0].x;
            center.y = frustum.corners[0].y;
            center.z = frustum.corners[0].z;
            center.x = center.x + frustum.corners[3].x;
            center.y = center.y + frustum.corners[3].y;
            center.z = center.z + frustum.corners[3].z;
            center.x = center.x + frustum.corners[1].x;
            center.y = center.y + frustum.corners[1].y;
            center.z = center.z + frustum.corners[1].z;
            center.x = center.x + frustum.corners[2].x;
            center.y = center.y + frustum.corners[2].y;
            center.z = center.z + frustum.corners[2].z;
            center.x = center.x * 0.25f;
            center.y = center.y * 0.25f;
            center.z = center.z * 0.25f;
            CWorldScene::CreateOcclusionVolumes(&center, frustum.corners, 1);
            CShadowQuery::QueryMapObjDefs(data, 0, data.m_lastBand);
            CShadowQuery::QueryTerrain(data, 0, data.m_lastBand);
        }
    } else {
        for (int32_t band = 0; band <= data.m_lastBand; band++) {
            if (flags & 0xC) {
                CWorldOcclusion::ClearVolumes();
                CShadowQuery::QueryMapObjDefsBand(data, band);
                CShadowQuery::QueryTerrainBand(data, band);
            }
        }
    }

    return 1;
}

// Not stock (shadowTerrainCast): ADT chunks cast into the band, depth only through the WMO untextured shadow path.
// Only the triangles facing away from the light are drawn: their depth is where a ray leaves the land, so a hill
// still shades what lies behind it, while a slope facing the light never shades itself (no acne there) and flat
// ground costs nothing. Away-facing slopes are unlit by N.L anyway. Every band uses the rendered 4 triangles a quad
// (shadow audit V1: band 2's old 2-triangle corner grid stood off the surface by the centre vertex's dip).
// Positions go in camera-relative (the band's view is camera-relative), pushed back along the light by two texels:
// that is depth bias only, the shadow does not move.
static void GetChunkRect(const CAaBox& box, CiRect& rect);
static const uint32_t kTerrainBatchChunks = 64;    // 64 * 145 * 36 B = 334 KB of the 2.8 MB vertex stream pool
static uint32_t s_terrainTris[3];
static uint32_t s_terrainDraws[3];

// shadow audit P1: a chunk's caster (its 145 positions and away-facing triangles) depends on the sun alone, so it is
// kept per chunk until the sun turns ~1 degree or the map reloads; entries not drawn for kTerrainCacheFrames go.
struct TerrainCaster {
    const float* height;    // the chunk's identity with its pointer (a freed chunk's address can come back)
    C3Vector topLeft;
    int32_t frame;
    C3Vector p[145];
    std::vector<uint16_t> indices;
};
static const int32_t kTerrainCacheFrames = 600;
static std::unordered_map<const CMapChunk*, TerrainCaster> s_terrainCache;
static C3Vector s_terrainCacheSun;
static uint32_t s_terrainCacheLoads;

// Far tree impostors stay out of the unit map unless shadowUnitFarTrees: Update draws it through band 0's slot with
// flags 13 at q5, so without this it took them at 40 yd wide and up to 40 / sin 4 deg = 573 yd along the sun, with
// 1/2 of band 0's texel and bias, redrawn round the camera target each frame (treeshadow3). Terrain does cast into it
// (unitterrain): the unit map replaces the bands inside its square, so without the land there it cut a lit diamond
// out of a hill's shadow and let trees beyond a ridge shade the near side.
static bool UnitMapExcluded() {
    return CShadowCache::s_drawingUnitMap && !(s_cvShadowUnitFarTrees && s_cvShadowUnitFarTrees->m_intValue);
}

static bool TerrainCastWanted(const CShadowCacheQueryData& data, int32_t band) {
    return s_cvShadowTerrainCast && s_cvShadowTerrainCast->m_intValue && band >= 0 && band < 3 && (data.m_queryFlags[band] & 0x8);
}

static void RenderTerrainShadow(CShadowCacheQueryData& data, int32_t band, const C3Vector& camera) {
    s_terrainTris[band] = 0;
    s_terrainDraws[band] = 0;

    const C3Vector& sun = CShadowCache::s_sunDir;    // light travel direction
    const float texel = CShadowCache::s_textureSize[0] > 0.0f ? 2.0f * data.m_radius[band] / CShadowCache::s_textureSize[0] : 0.5f;
    const float push = 2.0f * texel + (band >= 2 ? 1.0f : 0.0f);

    if (sun.x * s_terrainCacheSun.x + sun.y * s_terrainCacheSun.y + sun.z * s_terrainCacheSun.z < 0.99985f || s_terrainCacheLoads != g_lowDetailLoads) {
        s_terrainCache.clear();
        s_terrainCacheSun = sun;
        s_terrainCacheLoads = g_lowDetailLoads;
    } else if ((CWorld::frameCnt & 255) == 0) {
        for (auto it = s_terrainCache.begin(); it != s_terrainCache.end();) {
            it = CWorld::frameCnt - it->second.frame > kTerrainCacheFrames ? s_terrainCache.erase(it) : std::next(it);
        }
    }
    const C3Vector shift(sun.x * push - camera.x, sun.y * push - camera.y, sun.z * push - camera.z);

    CAaBox& extents = data.m_extents[band];
    CiRect rect;
    rect.minY = rect.minX = rect.maxY = rect.maxX = 0;
    GetChunkRect(extents, rect);

    g_theGxDevicePtr->RsPush();
    GxXformSet(GxXform_World, C44Matrix());
    CShaderEffect::UpdateWorldViewMatrix();
    CShaderEffect::SetAlphaRef(0.0f);
    CShaderEffect::SetDefaultShaders(0);
    GxRsSet(GxRs_Texture0, 0);
    float constants[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, constants, 1);

    CGxVertexPNCT* verts = nullptr;
    uint16_t* indices = nullptr;
    CGxBuf* vbuf = nullptr;
    CGxBuf* ibuf = nullptr;
    uint32_t vcount = 0, icount = 0, chunks = 0;

    auto flush = [&]() {
        if (!vbuf) {
            return;
        }

        g_theGxDevicePtr->BufUnlock(vbuf, 0);
        g_theGxDevicePtr->BufUnlock(ibuf, 0);
        vbuf->m_hasData = 1;
        ibuf->m_hasData = 1;

        if (icount) {
            GxPrimVertexPtr(vbuf, GxVBF_PNCT);
            g_theGxDevicePtr->PrimIndexPtr(ibuf);
            CGxBatch batch;
            batch.m_primType = GxPrim_Triangles;
            batch.m_start = 0;
            batch.m_count = icount;
            batch.m_minIndex = 0;
            batch.m_maxIndex = static_cast<uint16_t>(vcount - 1);
            g_theGxDevicePtr->Draw(&batch, 1);
            s_terrainTris[band] += icount / 3;
            s_terrainDraws[band]++;
        }

        vbuf = ibuf = nullptr;
        vcount = icount = chunks = 0;
    };

    // keeps triangle (a, b, c) of the chunk's 145 vertices when its face looks away from the light. Vertices whose
    // smooth normal (MCNR, 1/127 units, the one the ground is shaded with) faces the sun are pushed kLitPush yd further
    // along it: a small bump's back triangle then no longer shades the lit-shaded triangles behind it (hard-edged dark
    // triangles near the terminator), yet stays in the caster. Dropping those triangles instead (ridgecast) left holes
    // at every crest: lit diamonds in a hill's shadow, and trees beyond a ridge shading its near side through them.
    const float kLitPush = 4.0f;    // one ADT vertex spacing (4.17 yd)
    auto lit = [&](const int8_t* nrm, int32_t v) {
        return nrm && nrm[3 * v] * sun.x + nrm[3 * v + 1] * sun.y + nrm[3 * v + 2] * sun.z < 0.0f;
    };
    auto tri = [&](const C3Vector* p, std::vector<uint16_t>& out, int32_t a, int32_t b, int32_t c) {
        C3Vector e1 = p[b] - p[a];
        C3Vector e2 = p[c] - p[a];
        C3Vector n(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);

        if (n.z < 0.0f) {
            n = C3Vector(-n.x, -n.y, -n.z);
        }

        if (n.x * sun.x + n.y * sun.y + n.z * sun.z < 0.0f) {
            return;
        }

        out.push_back(static_cast<uint16_t>(a));
        out.push_back(static_cast<uint16_t>(b));
        out.push_back(static_cast<uint16_t>(c));
    };

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk || !chunk->m_height || !chunk->m_header || chunk->m_header->m_holes == 0xFFFF || !extents.Intersects(chunk->m_bbox)) {
                continue;
            }

            if (!data.m_bandFrustums[band].Cull(chunk->m_bbox) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(chunk->m_bbox) == WorldCull_notOutside)) {
                continue;
            }

            TerrainCaster& caster = s_terrainCache[chunk];

            if (caster.height != chunk->m_height || !(caster.topLeft == chunk->m_topLeftCoords)) {
                caster.height = chunk->m_height;
                caster.topLeft = chunk->m_topLeftCoords;
                caster.indices.clear();

                for (int32_t i = 0; i < 145; i++) {
                    chunk->GetVertexPosition(i, &caster.p[i]);

                    if (lit(chunk->m_normals, i)) {
                        caster.p[i] = caster.p[i] + sun * kLitPush;
                    }
                }

                const uint32_t holes = chunk->m_header->m_holes;

                for (int32_t r = 0; r < 8; r++) {
                    for (int32_t c = 0; c < 8; c++) {
                        if (holes & CMap::s_holeMask[(r / 2) * 4 + (c / 2)]) {
                            continue;
                        }

                        const int32_t tl = r * 17 + c, tr = tl + 1, ctr = tl + 9, bl = tl + 17, br = tl + 18;
                        tri(caster.p, caster.indices, ctr, tl, bl);
                        tri(caster.p, caster.indices, ctr, tr, tl);
                        tri(caster.p, caster.indices, ctr, br, tr);
                        tri(caster.p, caster.indices, ctr, bl, br);
                    }
                }
            }

            caster.frame = CWorld::frameCnt;

            if (caster.indices.empty()) {
                continue;   // all of it faces the light: casts nothing
            }

            if (!vbuf) {
                vbuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPNCT), kTerrainBatchChunks * 145);
                ibuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, kTerrainBatchChunks * 768);
                verts = reinterpret_cast<CGxVertexPNCT*>(g_theGxDevicePtr->BufLock(vbuf));
                indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(ibuf));
            }

            for (uint16_t i : caster.indices) {
                indices[icount++] = static_cast<uint16_t>(vcount + i);
            }

            const C3Vector* p = caster.p;
            CGxVertexPNCT* v = verts + vcount;

            for (int32_t i = 0; i < 145; i++) {
                v[i].m_position = p[i] + shift;
                v[i].m_normal = C3Vector(0.0f, 0.0f, 1.0f);
                v[i].m_color = CImVector(0xFF, 0xFF, 0xFF, 0xFF);
                v[i].m_texture = C2Vector(0.0f, 0.0f);
            }

            vcount += 145;

            if (++chunks == kTerrainBatchChunks) {
                flush();
            }
        }
    }

    flush();
    g_theGxDevicePtr->RsPop();
}

// VERIFY: 2abab737 manual
// OFFSET: 0x007BBC50
int32_t CShadowQuery::RenderCallback(CShadowCacheQueryData& data, int32_t band, CGxTex* target, CGxTex* colorTarget, C3Vector& direction) {
    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

    const bool terrain = TerrainCastWanted(data, band);
    // Not stock (farTreesShadow): the far tree impostors past their fade cast into the cascades as the trees would.
    const bool farTrees = !UnitMapExcluded() && band >= 0 && band < 3 && (data.m_queryFlags[band] & 0x8) && FarTrees::ShadowWanted();

    if (!buffers.m_groupCount && !buffers.m_untextured.m_count && !buffers.m_textured.m_count && !terrain && !farTrees) {
        CShadowQuery::Clear(target, colorTarget, data.m_texBounds[band]);
        return 1;
    }

    if (colorTarget) {
        GxRenderTargetSet(GxBuffers_Depth, colorTarget, 0);
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    CShaderEffect::SetTexMtx_Identity(0);

    C44Matrix projection;
    C3Vector eye(0.0f, 0.0f, 0.0f);
    C3Vector center(0.0f, 0.0f, 0.0f);
    C44Matrix lookAt;
    C44Matrix view;
    C44Matrix inverseView;
    C3Vector camera(0.0f, 0.0f, 0.0f);
    CGxTex* oldTarget;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
    GxRsSet(GxRs_Fog, 0);

    CShaderEffect* effect = CShaderEffectManager::GetEffect("ShadowMapRenderSL");
    CGWorldFrame::GetCameraPosition(&camera);
    C3Vector& pos = data.m_pos[band];
    eye.x = pos.x - CShadowCache::s_sunDir.x * 2000.0f - camera.x;
    eye.y = pos.y - CShadowCache::s_sunDir.y * 2000.0f - camera.y;
    eye.z = pos.z - 2000.0f * CShadowCache::s_sunDir.z - camera.z;
    center.x = pos.x - camera.x;
    center.y = pos.y - camera.y;
    center.z = pos.z - camera.z;

    C44Matrix translation;
    translation.d0 = -camera.x;
    translation.d1 = -camera.y;
    translation.d2 = -camera.z;

    CGxDevice* device = g_theGxDevicePtr;
    projection = device->m_projection;
    float viewportMinX = device->m_viewport.m_x.m_l;
    float viewportMaxX = device->m_viewport.m_x.m_h;
    float viewportMinY = device->m_viewport.m_y.m_l;
    float viewportMaxY = device->m_viewport.m_y.m_h;
    float viewportMinZ = device->m_viewport.m_z.m_l;
    float viewportMaxZ = device->m_viewport.m_z.m_h;
    GxXformSetViewport(data.m_texBounds[band][0], data.m_texBounds[band][1], data.m_texBounds[band][2], data.m_texBounds[band][3], 0.0f, 1.0f);

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    view = stack.m_mtx[stack.m_level];
    C44Matrix copy = view;
    float determinant = copy.Determinant();
    inverseView = copy.Inverse(determinant);

    effect->SetCurrent();
    float constants[4] = { 0.0f, 0.0f, 0.0f, 0.00025000001f };
    CShadowCache::SetShadowScale(0.00025000001f);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, constants, 1);
    CShaderEffect::s_shadowValue = CShadowCache::GetShadowValue();
    g_theGxDevicePtr->XformSetProjection(data.m_matrices[band]);
    CShaderEffect::UpdateProjMatrix();
    GxRsSet(GxRs_ScissorTest, 0);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, target, 0);
    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, CImVector(0xFF, 0xFF, 0xFF, 0xFF));

    if (g_shadowScissor->m_intValue) {
        GxRsSet(GxRs_ScissorTest, 1);
        float* rect = data.m_cullRects[band];
        CRect scissor;
        scissor.minY = (rect[0] + 1.0f) * 0.5f;
        scissor.minX = (rect[1] + 1.0f) * 0.5f;
        scissor.maxY = (rect[2] + 1.0f) * 0.5f;
        scissor.maxX = 0.5f * (rect[3] + 1.0f);
        g_theGxDevicePtr->SetScissor(scissor);
    }

    GxuXformCreateLookAtXXX(eye, center, direction, lookAt);
    g_theGxDevicePtr->XformSetView(lookAt);
    C44Matrix texMatrix = (inverseView * lookAt).Transpose();
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 14, &texMatrix.a0, 3);

    if (buffers.m_groupCount) {
        CMapObj::RenderMapObjGroupsShadowMap(buffers.m_groups, buffers.m_groupCount, buffers.m_groupData, translation, data.m_frustums[band]);
    }

    if (buffers.m_untextured.m_count || buffers.m_textured.m_count) {
        if (g_shadowInstancing->m_intValue) {
            buffers.m_untextured.Optimize();
            buffers.m_textured.Optimize();
        }

        if (buffers.m_untextured.m_count > s_maxUntexturedBatches) {
            s_maxUntexturedBatches = buffers.m_untextured.m_count;
        }

        if (buffers.m_textured.m_count > s_maxTexturedBatches) {
            s_maxTexturedBatches = buffers.m_textured.m_count;
        }

        CM2Model::RenderModelBatchesShadowMap(buffers.m_untextured, buffers.m_textured);
    }

    if (terrain) {
        RenderTerrainShadow(data, band, camera);
    }

    if (farTrees) {
        FarTrees::DrawShadow(data.m_extents[band], data.m_bandFrustums[band], camera);
    }

    g_theGxDevicePtr->RsPop();
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    g_theGxDevicePtr->XformSetView(view);
    GxRsSet(GxRs_ScissorTest, 0);
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);
    g_theGxDevicePtr->XformSetProjection(projection);
    CShaderEffect::UpdateProjMatrix();
    return 1;
}

/// Not stock (shadowCasterMinTexels): the stock minimum caster radius of bands 1-2 (2 and 10 yd) drops whole
/// houses' worth of fences, carts and bushes that still span several texels; the minimum is that many band
/// texels instead, never above stock.
static float CasterMinRadius(int32_t band, float stock) {
    float texels = s_cvShadowCasterMinTexels ? s_cvShadowCasterMinTexels->m_floatValue : 0.0f;

    if (band <= 0 || texels <= 0.0f || !CShadowCache::s_shadowSize) {
        return stock;
    }

    float minimum = texels * 2.0f * CShadowCache::s_extShadowBands[band].m_radius / CShadowCache::s_shadowSize;
    return minimum < stock ? minimum : stock;
}

// VERIFY: f464b8c3 manual
// OFFSET: 0x007BB9D0
void CShadowQuery::GatherShadowCasters(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    float maxRadius[3];
    float minRadius[3];
    maxRadius[0] = 10000.0f;

    if ((data.m_queryFlags[0] & 0xC) == 4) {
        maxRadius[0] = 25.0f;
    }

    minRadius[0] = 0.25f;
    maxRadius[1] = 10000.0f;
    minRadius[1] = CasterMinRadius(1, 2.0f);
    maxRadius[2] = 10000.0f;
    minRadius[2] = CasterMinRadius(2, 10.0f);

    for (CMapEntity* entity = CMap::entityList.Head(); entity; entity = CMap::entityList.Next(entity)) {
        if (entity->m_staticFlags & 0x4) {
            continue;
        }

        CM2Model* model = entity->m_model;

        if (!model) {
            continue;
        }

        int32_t isUnit = 1;

        if (!(entity->m_type & 0x20) || (entity->m_staticFlags & 0x2000)) {
            isUnit = 0;
        }

        uint32_t doodad = (model->m_fFlags & 0x1) ? model->m_shared->m_billboardBoneCount : 0;

        for (int32_t band = firstBand; band <= lastBand; band++) {
            int32_t queryFlags = data.m_queryFlags[band];
            uint32_t flags;

            if (queryFlags & 0x2) {
                if (!isUnit) {
                    continue;
                }

                flags = entity->m_flags;

                if (flags & 0x4) {
                    continue;
                }

                if (CShadowQuery::s_cutoffHeight < entity->m_position.z) {
                    continue;
                }
            } else {
                flags = entity->m_flags;

                if (flags & 0x2) {
                    if (!(entity->m_staticFlags & 0x1000)) {
                        continue;
                    }

                    if (CShadowQuery::s_cutoffHeight < entity->m_position.z) {
                        continue;
                    }
                }
            }

            int32_t wanted;

            if (isUnit) {
                wanted = queryFlags & 0x1;
            } else if (doodad) {
                wanted = queryFlags & 0x4;
            } else {
                wanted = queryFlags & 0x8;
            }

            if (!wanted) {
                continue;
            }

            CAaBox& extents = data.m_extents[band];
            CAaBox& bbox = entity->m_bbox;

            if (extents.t.x >= bbox.b.x && extents.t.y >= bbox.b.y && extents.t.z >= bbox.b.z && extents.b.x <= bbox.t.x && extents.b.y <= bbox.t.y && extents.b.z <= bbox.t.z && minRadius[band] <= entity->m_sphere.r && ((flags & 0x20000) || maxRadius[band] >= entity->m_sphere.r)) {
                if (data.m_bandFrustums[band].Cull(bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(bbox) != WorldCull_notOutside)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: 7278bb56 manual
// OFFSET: 0x007BB830
void CShadowQuery::Clear(CGxTex* target, CGxTex* depthTarget, float* rect) {
    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
    CGxDevice* device = g_theGxDevicePtr;
    float viewportMinX = device->m_viewport.m_x.m_l;
    float viewportMaxX = device->m_viewport.m_x.m_h;
    float viewportMinY = device->m_viewport.m_y.m_l;
    float viewportMaxY = device->m_viewport.m_y.m_h;
    float viewportMinZ = device->m_viewport.m_z.m_l;
    float viewportMaxZ = device->m_viewport.m_z.m_h;
    GxRenderTargetSet(GxBuffers_Depth, depthTarget, 0);
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    GxXformSetViewport(rect[0], rect[1], rect[2], rect[3], 0.0f, 1.0f);
    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, CImVector(0xFF, 0xFF, 0xFF, 0xFF));
    GxXformSetViewport(viewportMinX, viewportMaxX, viewportMinY, viewportMaxY, viewportMinZ, viewportMaxZ);
    GxRenderTargetSet(GxBuffers_Color, oldTarget, 0);
}

// VERIFY: 89a7afcf auto
// OFFSET: 0x007BA8F0
void CShadowQuery::GetSize(CShadowCacheQueryData& data, int32_t band, float* minSize, float* maxSize) {
    *maxSize = 10000.0f;

    if (band == 0) {
        if ((data.m_queryFlags[0] & 0xC) == 4) {
            *maxSize = 25.0f;
            *minSize = 0.25f;
            return;
        }

        *minSize = 0.25f;
        return;
    }

    if (band < 1) {
        *minSize = 0.25f;
        return;
    }

    *minSize = CasterMinRadius(band, band >= 2 ? 10.0f : 2.0f);
}

// VERIFY: 69b3a5e2 auto
// OFFSET: 0x007BABC0
bool CWorldScene::IsBeyondFadeDistance(const C3Vector& position, uint32_t fadeLevel) {
    float dz = position.z - CWorldScene::s_activeWorldView.z;
    float dy = position.y - CWorldScene::s_activeWorldView.y;
    float dx = position.x - CWorldScene::s_activeWorldView.x;
    return dx * dx + (dy * dy + dz * dz) > CWorldScene::s_fadeDistMinSqr[fadeLevel];
}

// VERIFY: 18313b4e auto
// OFFSET: 0x007BB460
static void GetChunkRect(const CAaBox& box, CiRect& rect) {
    float minY = -(box.t.x - 17066.666f);
    float minX = -(box.t.y - 17066.666f);
    float maxY = -(box.b.x - 17066.666f);
    float maxX = -(box.b.y - 17066.666f);

    if (minX < 0.0f) {
        minX = 0.0f;
    }

    if (minY < 0.0f) {
        minY = 0.0f;
    }

    if (maxX >= 34133.332f) {
        maxX = 34132.332f;
    }

    if (maxY >= 34133.332f) {
        maxY = 34132.332f;
    }

    int32_t cellMaxX = CMath::fint_(maxX * OO_COORD_TO_SUBCHUNK - 0.5f);
    int32_t cellMaxY = CMath::fint_(maxY * OO_COORD_TO_SUBCHUNK - 0.5f);
    int32_t cellMinX = CMath::fint_(minX * OO_COORD_TO_SUBCHUNK - 0.5f);
    int32_t cellMinY = CMath::fint_(minY * OO_COORD_TO_SUBCHUNK - 0.5f);
    rect.minY = cellMinY >> 3;
    rect.minX = cellMinX >> 3;
    rect.maxY = cellMaxY >> 3;
    rect.maxX = cellMaxX >> 3;
}

// VERIFY: 9b4413e5 manual
// OFFSET: 0x007BC890
void CShadowQuery::QueryMapObjDefsBand(CShadowCacheQueryData& data, int32_t band) {
    float minSize;
    float maxSize;
    CShadowQuery::GetSize(data, band, &minSize, &maxSize);

    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        uint32_t flags = def->m_flags;

        if (flags & 0x20) {
            continue;
        }

        float maxDoodadSize = maxSize;
        int32_t interior = 0;
        int32_t castDoodads = 0;
        int32_t castOther = 0;

        if (flags & 0x400) {
            if (!(data.m_queryFlags[band] & 0x4)) {
                continue;
            }

            maxDoodadSize = 100000.0f;
            castDoodads = 1;
            castOther = 1;
            interior = 1;
        } else {
            int32_t queryFlags = data.m_queryFlags[band];

            if (queryFlags & 0x8) {
                castOther = 1;
            }

            if (queryFlags & 0x4) {
                castDoodads = 1;
            }
        }

        CAaBox& extents = data.m_extents[band];

        if (!extents.Intersects(def->m_bbox)) {
            continue;
        }

        if (!data.m_bandFrustums[band].Cull(def->m_bbox) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(def->m_bbox) == WorldCull_notOutside)) {
            continue;
        }

        uint32_t groupCount = def->m_defGroups.m_count == 0xFFFFFFFF ? def->m_defGroups.m_dynamic.m_count : def->m_defGroups.m_count;

        for (uint32_t i = 0; i < groupCount; i++) {
            CMapObjDefGroup* defGroup = def->m_defGroups[i];
            CMapObjGroup* group = def->m_owner->GetGroup(i, 0);

            // shadowWmoInterior: interior-only groups cast too (outside walls, roofs and towers are often flagged
            // interior; caves lie under the terrain, which shadows them anyway), but their doodads do not (furniture
            // inside a closed shell adds batches and no shadow). Stock: exterior (0x8) and exterior-lit (0x40) only.
            const bool interiorGroup = group && !(group->m_flags & 0x48);

            if (!group || (interiorGroup && !CShadowCache::s_wmoInterior)) {
                continue;
            }

            if (!extents.Intersects(defGroup->m_bbox) || !data.m_bandFrustums[band].Cull(defGroup->m_bbox)) {
                continue;
            }

            if (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(defGroup->m_bbox) == WorldCull_notOutside) {
                continue;
            }

            if (interior ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8)) {
                ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

                if (buffers.m_groupCount >= kShadowGroupCap) {
                    break;
                }

                static_cast<C44Matrix**>(buffers.m_groupData)[buffers.m_groupCount] = &def->m_mat;
                static_cast<CMapObjGroup**>(buffers.m_groups)[buffers.m_groupCount++] = group;
            }

            if (interiorGroup) {
                continue;
            }

            for (CMapBaseObjLink* link = defGroup->m_doodadDefLinkList.Head(); link; link = defGroup->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                    continue;
                }

                doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80) || CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) || maxDoodadSize < doodad->m_sphere.r) {
                    continue;
                }

                int32_t wanted = model->GetBillboardBoneCount() ? castDoodads : castOther;

                if (wanted && extents.Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != WorldCull_notOutside)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: 34693e83 manual
// OFFSET: 0x007BC490
void CShadowQuery::QueryMapObjDefs(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        if (def->m_flags & 0x20) {
            continue;
        }

        CAaBox& lastExtents = data.m_extents[lastBand];

        if (!(lastExtents.t.x >= def->m_bbox.b.x && lastExtents.t.y >= def->m_bbox.b.y && lastExtents.t.z >= def->m_bbox.b.z && lastExtents.b.x <= def->m_bbox.t.x && lastExtents.b.y <= def->m_bbox.t.y && lastExtents.b.z <= def->m_bbox.t.z)) {
            continue;
        }

        CFrustum& lastFrustum = data.m_bandFrustums[lastBand];

        if (!lastFrustum.Cull(def->m_bbox)) {
            continue;
        }

        uint32_t groupCount = def->m_defGroups.m_count == 0xFFFFFFFF ? def->m_defGroups.m_dynamic.m_count : def->m_defGroups.m_count;

        for (uint32_t i = 0; i < groupCount; i++) {
            CMapObjDefGroup* defGroup = def->m_defGroups[i];
            CMapObjGroup* group = def->m_owner->GetGroup(i, 0);

            // shadowWmoInterior: interior-only groups cast too (outside walls, roofs and towers are often flagged
            // interior; caves lie under the terrain, which shadows them anyway), but their doodads do not (furniture
            // inside a closed shell adds batches and no shadow). Stock: exterior (0x8) and exterior-lit (0x40) only.
            const bool interiorGroup = group && !(group->m_flags & 0x48);

            if (!group || (interiorGroup && !CShadowCache::s_wmoInterior)) {
                continue;
            }

            if (!lastExtents.Intersects(defGroup->m_bbox) || !lastFrustum.Cull(defGroup->m_bbox)) {
                continue;
            }

            for (int32_t band = firstBand; band <= lastBand; band++) {
                // (not stock: a band kept from an earlier frame, query flags 0, gathers nothing; shadowCascadeReuse)
                if (data.m_queryFlags[band] && data.m_extents[band].Intersects(defGroup->m_bbox) && data.m_bandFrustums[band].Cull(defGroup->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(defGroup->m_bbox) != WorldCull_notOutside)) {
                    ShadowQueryBuffers& buffers = CShadowQuery::s_bandBuffers[band];

                    if (buffers.m_groupCount >= kShadowGroupCap) {
                        break;
                    }

                    static_cast<C44Matrix**>(buffers.m_groupData)[buffers.m_groupCount] = &def->m_mat;
                    static_cast<CMapObjGroup**>(buffers.m_groups)[buffers.m_groupCount++] = group;
                }
            }

            if (interiorGroup) {
                continue;
            }

            for (CMapBaseObjLink* link = defGroup->m_doodadDefLinkList.Head(); link; link = defGroup->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (!doodad->m_model || !(doodad->m_flags & 0x80)) {
                    continue;
                }

                for (int32_t band = firstBand; band <= lastBand; band++) {
                    if (!data.m_queryFlags[band] || doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                        continue;
                    }

                    doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;

                    if (!CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) && data.m_extents[band].Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != WorldCull_notOutside)) {
                        doodad->m_model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                    }
                }
            }
        }
    }
}

// VERIFY: 896bfb08 manual
// OFFSET: 0x007BCF20
void CShadowQuery::QueryTerrainBand(CShadowCacheQueryData& data, int32_t band) {
    float minSize;
    float maxSize;
    CShadowQuery::GetSize(data, band, &minSize, &maxSize);
    CAaBox& extents = data.m_extents[band];
    CiRect rect;
    rect.minY = 0;
    rect.minX = 0;
    rect.maxY = 0;
    rect.maxX = 0;
    GetChunkRect(extents, rect);

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk) {
                continue;
            }

            if (!(extents.t.x >= chunk->m_bbox.b.x && extents.t.y >= chunk->m_bbox.b.y && extents.t.z >= chunk->m_bbox.b.z && extents.b.x <= chunk->m_bbox.t.x && extents.b.y <= chunk->m_bbox.t.y && extents.b.z <= chunk->m_bbox.t.z)) {
                continue;
            }

            if (!data.m_bandFrustums[band].Cull(chunk->m_bbox) || (data.m_useCullFrustum[band] && data.m_cullFrustums[band].CullInv(chunk->m_bbox) == WorldCull_notOutside)) {
                continue;
            }

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);

                if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                    continue;
                }

                doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80) || CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel) || maxSize < doodad->m_sphere.r) {
                    continue;
                }

                int32_t wanted = model->GetBillboardBoneCount() ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8);

                if (wanted && extents.Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != WorldCull_notOutside)) {
                    model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                }
            }
        }
    }
}

// VERIFY: ba8a3951 manual
// OFFSET: 0x007BCC00
void CShadowQuery::QueryTerrain(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand) {
    CiRect rect;
    rect.minY = 0;
    rect.minX = 0;
    rect.maxY = 0;
    rect.maxX = 0;
    GetChunkRect(data.m_extents[data.m_lastBand], rect);

    for (int32_t row = rect.minY; row <= rect.maxY; row++) {
        for (int32_t col = rect.minX; col <= rect.maxX; col++) {
            CMapArea* area = CMap::m_areaTable[(((row >> 4) & 0x3F) << 6) + ((col >> 4) & 0x3F)];

            if (!area || area->m_asyncObject) {
                continue;
            }

            CMapChunk* chunk = area->m_mapChunks[(col & 0xF) + 16 * (row & 0xF)];

            if (!chunk) {
                continue;
            }

            CAaBox& lastExtents = data.m_extents[lastBand];

            if (!(lastExtents.t.x >= chunk->m_bbox.b.x && lastExtents.t.y >= chunk->m_bbox.b.y && lastExtents.t.z >= chunk->m_bbox.b.z && lastExtents.b.x <= chunk->m_bbox.t.x && lastExtents.b.y <= chunk->m_bbox.t.y && lastExtents.b.z <= chunk->m_bbox.t.z)) {
                continue;
            }

            if (!data.m_bandFrustums[lastBand].Cull(chunk->m_bbox) || CWorldOcclusion::QueryVolumes(&chunk->m_sphere)) {
                continue;
            }

            for (CMapBaseObjLink* link = chunk->m_doodadDefLinkList.Head(); link; link = chunk->m_doodadDefLinkList.Next(link)) {
                CMapDoodadDef* doodad = static_cast<CMapDoodadDef*>(link->m_owner);
                CM2Model* model = doodad->m_model;

                if (!model || !(doodad->m_flags & 0x80)) {
                    continue;
                }

                uint32_t doodadFlag = model->GetBillboardBoneCount();

                for (int32_t band = firstBand; band <= lastBand; band++) {
                    if (doodad->m_shadowStamps[band] == CShadowQuery::s_stamp) {
                        continue;
                    }

                    doodad->m_shadowStamps[band] = CShadowQuery::s_stamp;

                    if (CWorldScene::IsBeyondFadeDistance(doodad->m_sphere.c, doodad->m_fadeLevel)) {
                        continue;
                    }

                    if (!(doodadFlag ? (data.m_queryFlags[band] & 0x4) : (data.m_queryFlags[band] & 0x8))) {
                        continue;
                    }

                    if (data.m_extents[band].Intersects(doodad->m_bbox) && data.m_bandFrustums[band].Cull(doodad->m_bbox) && (!data.m_useCullFrustum[band] || data.m_cullFrustums[band].CullInv(doodad->m_bbox) != WorldCull_notOutside)) {
                        model->GatherModelBatchesShadowMap(CShadowQuery::s_bandBuffers[band].m_untextured, CShadowQuery::s_bandBuffers[band].m_textured);
                    }
                }
            }
        }
    }
}
