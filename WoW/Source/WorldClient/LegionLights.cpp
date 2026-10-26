// LEGION lights: not in 12340. Legion 7.3.5 lightMode / maxLightCount / maxLightDist (survey 2.8) as a v1 screen-space
// lamp pass: the CM2Scene light grid's visible point lights (the same list vfog's fog scattering walks, without its
// 8-light cap) added onto the frame from the scene depth, colour += colour * light (GxBlend_ModAdd). Shader
// LegionLights.hlsl; falloff after northlight (MIT). docs/LEGION_PORT.md "Legion lights".
// ponytail: screen space after the receivers, not a light pre-pass read by them; see the docs for the next phase.
#include "WoW/Source/WorldClient/LegionLights.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2ModelLight.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include "WoW/Source/WorldClient/World.h"

namespace LegionLights {

namespace {

using namespace VolumetricFog;

constexpr int kLightsPerPass = 16;          // LegionLights.hlsl kLights
constexpr int kMaxLights = 256;             // maxLightCount's range (BENILLA light grid: the shared table's 256)
// BENILLA light grid (light_bin.wgsl, locallight.wgsl): the world viewport in kGridX x kGridY tiles, each light binned
// into the tiles its sphere's screen box touches; a tile draws only its own lights, at most kPerTile of them (benilla
// PER_CELL 8 a world cell; ours two of the shader's 16-light passes), the strongest kept when one overflows.
constexpr int kGridX = 16;
constexpr int kGridY = 9;
constexpr int kPerTile = 32;
constexpr uint32_t kControlRegister = 10;   // c10 control, c11.. three registers a light
constexpr uint32_t kShadowRegister = 59;    // c59 shadows (LIGHTOCC)
constexpr float kShadowDarkness = 0.85f;    // a fully blocked lamp keeps 15 % of its light (soft, never black)
constexpr float kShadowMarch = 12.0f;       // yd: longest ray toward a lamp
constexpr float kShadowThickness = 2.5f;    // yd: what the depth buffer's front surfaces are taken to be deep
constexpr float kShadowDist = 120.0f;       // yd: no shadow rays past this
constexpr float kMaxRadius = 40.0f;         // yd
constexpr float kCutoff = 1.0f / 32.0f;     // the radius: where the stock attenuation leaves 1/32 of the colour
constexpr float kMaxColour = 4.0f;
constexpr float kMinLightLevel = 0.2f;

CVar* s_mode;
CVar* s_count;
CVar* s_dist;
CVar* s_intensity;
CVar* s_normalBuffer;
CVar* s_shadows;
CVar* s_grid;
CGxShader* s_shader;    // ponytail: not destroyed at device shutdown (one shader, the device goes with the process)
bool s_noShader;        // LegionLights.bls failed to load: the stock per-vertex lights stay

float Var(CVar* v, float fallback, float lo, float hi) {
    return v && std::isfinite(v->m_floatValue) ? std::clamp(v->m_floatValue, lo, hi) : fallback;
}

float Luma(const CImVector& c) {
    return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
}

struct Light {
    float view[3];
    float radius;
    float colour[3];
    float att[3];
    float distance;
    float score;
    const CM2Light* key;    // a static lamp's (s_static), else null
    int32_t slot;           // its baked occlusion, -1 none
};

// LIGHTOCC (Undercity lanterns dark from afar): a doodad's lights are refreshed only while its model animates, and
// the world animates a doodad only while it is drawn (AddDoodadDefModelToModelScene: doodad fade distance and the
// Legion objcull pixel size, both from the camera) or within 10 yd of the camera (World.cpp, frustum-culled doodads).
// Past that the light keeps its stamp and SelectLights clears m_visible: the lamp goes dark with the camera's
// distance or zoom. So the pass keeps what each light was on its last refreshed frame and lights it on from that
// while it stays linked in the grid (the model is alive) and never moved (a static doodad, not a carried torch).
struct Seen {
    C3Vector pos;
    uint32_t visible : 1;
    uint32_t moved : 1;
    uint32_t refreshes : 30;
    uint32_t walk;          // the last gather that met it in the grid
};
std::unordered_map<const CM2Light*, Seen> s_seen;
uint32_t s_walk;

// LIGHTBAKE (static light list): every placed doodad's M2 point lights, read from its model's light block when the
// doodad is placed (CMapDoodadDef::UpdateBounds, ADT MDDF and WMO MODD alike) and dropped with it (~CMapDoodadDef),
// in world space through the doodad's matrix, colour from the tracks' first keys. Keyed by the model's own CM2Light.
// LAMPSON: a placed lamp is ALWAYS on at this colour. Its live light only moves it, on the frames its model animated
// (stamp == frame stamp and visible); it never turns it off: stock CM2Scene::SelectLights UNLINKED a stale light
// from the grid (SetVisible(0) -> Unlink; since lightstay it no longer does) as soon as any model near it was lit, so "live wins" read every
// lamp whose doodad had once animated and was then culled (camera passed under it) as switched off. WMO MOLT lights are not read: 12340 never loads them (baked into MOCV), and the lamps they
// stand for are doodads with their own M2 lights.
struct Static {
    const CMapDoodadDef* def;
    C3Vector pos;
    float colour[3];
    int32_t slot;           // baked occlusion in the atlas, -1 none
    uint32_t used;          // the last frame it was selected (atlas LRU)
    bool noBake;            // the bake found the lamp shut in its own holder's collision: screen-space rays only
    bool emissive;          // EMISSIVE: derived from the model's glow (key is a fake, never dereferenced)
};
std::unordered_map<const CM2Light*, Static> s_static;
std::unordered_map<const CMapDoodadDef*, std::vector<const CM2Light*>> s_staticByDoodad;
CVar* s_emissive;
std::unordered_set<const CMapDoodadDef*> s_pending;    // EMISSIVE: placed before their skin profile arrived

// LIGHTBAKE (baked shadows): per static lamp a cube of kFace^2 rays a face cast once through the world's collision
// (World::Intersect: terrain, WMOs, doodads), the hit distance / kMaxRadius in 8 bits, into an atlas of kColumns slots
// a row (6 faces side by side); LRU over the slots, at most kBakeMs of rays a frame. LegionLights.hlsl BakedVisibility.
// ponytail: collision geometry, not a rendered cube depth map (no rig to verify a GPU bake on three backends, and
// this fits ps_3_0 everywhere); props without collision (fences, banners) are left to the screen-space rays.
constexpr int kFace = 16;
constexpr int kColumns = 10;                // 10 x 6 x 16 = 960 of a 1024 wide atlas
constexpr int kAtlasW = 1024;
constexpr int kRays = 6 * kFace * kFace;
constexpr double kBakeMs = 1.0;
constexpr float kBakeStart = 0.5f;          // yd: rays start this far out (the lamp's own flame and wick)
constexpr float kBakeShut = 1.0f;           // yd: more than half the rays blocked inside this = shut in its holder
constexpr uint32_t kBakeRegister = 60;      // c60 (1 / atlas w, 1 / atlas h, face texels, darkness)
constexpr uint32_t kIntersectFlags = 0x111; // terrain, WMO, doodad collision (Missile_C's static world)
CVar* s_baked;
CVar* s_atlasSize;
CGxTex* s_atlas;
int s_atlasH;
std::vector<uint32_t> s_atlasTexels;
std::vector<const CM2Light*> s_slots;       // owner per slot
uint32_t s_frame;
struct Job {
    const CM2Light* key = nullptr;
    int32_t slot = -1;
    int next = 0;
    int shut = 0;
    float radius = 0.0f;    // yd: the bake's reach, the lamp's radius when it started
    uint8_t dist[kRays];
} s_job;

void AtlasCallback(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t mipLevel, void*, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch && mipLevel == 0 && !s_atlasTexels.empty()) {
        texels = s_atlasTexels.data();
        stride = width * 4;
    }
}

void FreeSlot(int32_t slot) {
    if (slot >= 0 && slot < static_cast<int32_t>(s_slots.size())) {
        s_slots[slot] = nullptr;
    }
}

void DropAtlas() {
    DestroyTex(s_atlas);
    s_atlasH = 0;
    s_slots.clear();
    s_atlasTexels.clear();
    s_job.key = nullptr;
    for (auto& entry : s_static) {
        entry.second.slot = -1;
    }
}

// The atlas at lightBakedShadowAtlas rows; a new size starts it over.
bool PrepareAtlas() {
    int h = static_cast<int>(Var(s_atlasSize, 512.0f, 64.0f, 4096.0f));
    int pow2 = 64;
    while (pow2 < h) {
        pow2 <<= 1;
    }
    if (s_atlas && s_atlasH == pow2) {
        return true;
    }
    DropAtlas();
    s_atlasTexels.assign(static_cast<size_t>(kAtlasW) * pow2, 0xFFFFFFFFu);
    CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 0, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, kAtlasW, pow2, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, AtlasCallback, "LegionLightsBake", s_atlas);
    if (!s_atlas) {
        s_atlasTexels.clear();
        return false;
    }
    s_atlasH = pow2;
    s_slots.assign(static_cast<size_t>(kColumns) * (pow2 / kFace), nullptr);
    return true;
}

// Ray r of the cube: face f = r / kFace^2 (axis f / 2, sign by f & 1), texel (i, j); LegionLights.hlsl CubeTexel.
C3Vector RayDir(int r) {
    const int f = r / (kFace * kFace);
    const int i = r % kFace;
    const int j = (r / kFace) % kFace;
    float d[3];
    const int a = f >> 1;
    d[a] = (f & 1) ? -1.0f : 1.0f;
    d[(a + 1) % 3] = (i + 0.5f) * (2.0f / kFace) - 1.0f;
    d[(a + 2) % 3] = (j + 0.5f) * (2.0f / kFace) - 1.0f;
    const float n = 1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return C3Vector(d[0] * n, d[1] * n, d[2] * n);
}

// The texels of slot s, face f: x = (s % kColumns) * 6 kFace + f kFace, y = (s / kColumns) kFace.
int SlotX(int32_t slot) {
    return (slot % kColumns) * 6 * kFace;
}

int SlotY(int32_t slot) {
    return (slot / kColumns) * kFace;
}

int32_t AllocSlot() {
    int32_t best = -1;
    uint32_t oldest = UINT32_MAX;
    for (size_t s = 0; s < s_slots.size(); ++s) {
        const CM2Light* owner = s_slots[s];
        if (!owner) {
            return static_cast<int32_t>(s);
        }
        auto it = s_static.find(owner);
        const uint32_t used = it == s_static.end() ? 0 : it->second.used;
        if (owner != s_job.key && used != s_frame && used < oldest) {
            oldest = used;
            best = static_cast<int32_t>(s);
        }
    }
    if (best >= 0) {
        auto it = s_static.find(s_slots[best]);
        if (it != s_static.end()) {
            it->second.slot = -1;
        }
        s_slots[best] = nullptr;
    }
    return best;
}

// Up to kBakeMs of rays for the job; finished, its slot goes up to the atlas.
void RunBake() {
    const float radius = s_job.radius;
    auto it = s_static.find(s_job.key);
    if (it == s_static.end()) {
        FreeSlot(s_job.slot);
        s_job.key = nullptr;
        return;
    }
    Static& lamp = it->second;
    const auto start = std::chrono::steady_clock::now();
    for (; s_job.next < kRays; ++s_job.next) {
        if ((s_job.next & 15) == 0 && s_job.next != 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > kBakeMs) {
            return;
        }
        const C3Vector d = RayDir(s_job.next);
        const C3Vector from(lamp.pos.x + d.x * kBakeStart, lamp.pos.y + d.y * kBakeStart, lamp.pos.z + d.z * kBakeStart);
        const C3Vector to(lamp.pos.x + d.x * radius, lamp.pos.y + d.y * radius, lamp.pos.z + d.z * radius);
        C3Vector hit(0.0f, 0.0f, 0.0f);
        float t = 1.0f;
        float dist = radius;   // (a miss: 255, open out to kMaxRadius)
        if (World::Intersect(from, to, &hit, &t, kIntersectFlags, nullptr) && std::isfinite(t)) {
            dist = kBakeStart + std::clamp(t, 0.0f, 1.0f) * (radius - kBakeStart);
            s_job.shut += dist < kBakeShut;
        }
        s_job.dist[s_job.next] = t < 1.0f ? static_cast<uint8_t>(std::lround(std::clamp(dist / kMaxRadius, 0.0f, 1.0f) * 254.0f)) : 255;
    }
    if (s_job.shut * 2 > kRays) {
        lamp.noBake = true;
        FreeSlot(s_job.slot);
    } else {
        const int x0 = SlotX(s_job.slot);
        const int y0 = SlotY(s_job.slot);
        for (int r = 0; r < kRays; ++r) {
            const int f = r / (kFace * kFace);
            const uint32_t v = s_job.dist[r];
            s_atlasTexels[static_cast<size_t>(y0 + (r / kFace) % kFace) * kAtlasW + x0 + f * kFace + r % kFace] = v * 0x01010101u;
        }
        GxTexUpdate(s_atlas, x0, y0, x0 + 6 * kFace, y0 + kFace, 1);
        lamp.slot = s_job.slot;
    }
    s_job.key = nullptr;
}

// The track's first key: its global sequence's, or sequence 0's when that sequence lives in the .m2 (flag 0x20; an
// .anim one's keys are not relocated until it streams in, M2Shared.cpp M2Init, so the fallback stands).
template <class T>
T FirstKey(const M2Data& data, const M2Track<T>& track, T fallback) {
    const bool loaded = track.m_loopIndex != 0xFFFF || (data.m_sequences.m_count && (data.m_sequences[0].m_flags & 0x20));
    return loaded && track.m_sequenceKeys.m_count && track.m_sequenceKeys[0].m_count ? track.m_sequenceKeys[0][0] : fallback;
}

// EMISSIVE (benilla-bourbon crates/benilla-world/src/modern/lights/emissive.rs, 1:1): a placed doodad that authors
// no casting light but draws a flame or a glow (unlit additive particles, additive cards, a fixture's unlit glass)
// gets one derived point light at the glowing parts' centroid, colour = unit-peak mean x intensity scale, which
// stands for diffuse colour x diffuse intensity as an authored M2 light's does.
// ponytail: bones at rest (no billboard carrier shift), no flame flicker, no WMO window glows (v1).
namespace Emissive {
constexpr float kMinEmission = 0.02f;
constexpr float kReferenceSize = 0.35f;
constexpr float kEmissionToIntensity = 0.55f;
constexpr float kMaxIntensity = 1.5f;
constexpr float kUntintedGlow[3] = { 1.0f, 0.72f, 0.38f };
constexpr float kUntintedLimit = 0.08f;
constexpr float kGlowWeight = 0.9f;
constexpr float kReferenceCard = 0.5f;
constexpr float kMinFlameSize = 0.04f;
constexpr float kEmitterReach = 2.5f;

struct Sum {
    float weight = 0.0f;
    float rgb[3] = {};
    void Add(const float c[3], float w) {
        weight += w;
        for (int i = 0; i < 3; ++i) {
            rgb[i] += c[i] * w;
        }
    }
};

std::string Lower(const char* path) {
    std::string s(path);
    for (char& c : s) {
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool Contains(const std::string& s, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (s.find(k) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The track's peak over its rest keys (benilla shown_at_rest), fallback when it has none loaded.
float Peak(const M2Data& data, const M2Track<fixed16>& track, float fallback) {
    const bool loaded = track.m_loopIndex != 0xFFFF || (data.m_sequences.m_count && (data.m_sequences[0].m_flags & 0x20));
    if (!loaded || !track.m_sequenceKeys.m_count || !track.m_sequenceKeys[0].m_count) {
        return fallback;
    }
    float peak = -1e30f;
    const M2Array<fixed16>& keys = track.m_sequenceKeys[0];
    for (uint32_t i = 0; i < keys.m_count; ++i) {
        peak = std::max(peak, keys[i].m_value / 32767.0f);
    }
    return peak;
}

// emission_of: an unlit additive particle's opacity-weighted ramp mean, weighted by size and opacity.
bool EmissionOf(const M2Particle& p, Sum& sum) {
    if ((p.m_blendMode != M2PARTICLEBLEND_NO_ALPHA_ADD && p.m_blendMode != M2PARTICLEBLEND_ADD) || !(p.m_flags & 1)) {
        return false;   // not Add, or lit (M2Model: material flag 1 = lit iff file flag 0x1 clear)
    }
    const uint32_t n = std::min(p.m_colorTrack.m_values.m_count, p.m_alphaTrack.m_values.m_count);
    if (!n) {
        return false;
    }
    float alphaSum = 0.0f;
    float rgb[3] = {};
    for (uint32_t k = 0; k < n; ++k) {
        const float a = std::clamp(p.m_alphaTrack.m_values[k].m_value / 32767.0f, 0.0f, 1.0f);
        const C3Vector& c = p.m_colorTrack.m_values[k];
        alphaSum += a;
        rgb[0] += std::max(c.x / 255.0f, 0.0f) * a;
        rgb[1] += std::max(c.y / 255.0f, 0.0f) * a;
        rgb[2] += std::max(c.z / 255.0f, 0.0f) * a;
    }
    if (alphaSum <= 0.0f) {
        return false;
    }
    const float mean[3] = { rgb[0] / alphaSum, rgb[1] / alphaSum, rgb[2] / alphaSum };
    const float luminance = 0.2126f * mean[0] + 0.7152f * mean[1] + 0.0722f * mean[2];
    float size = 0.0f;
    for (uint32_t k = 0; k < p.m_scaleTrack.m_values.m_count; ++k) {
        size = std::max(size, p.m_scaleTrack.m_values[k].x);
    }
    if (size < kMinFlameSize) {
        return false;
    }
    const float weight = luminance * std::clamp(size / kReferenceSize, 0.25f, 3.0f) * (alphaSum / n);
    if (weight < kMinEmission) {
        return false;
    }
    sum.Add(mean, weight);
    return true;
}

// glow_emission: an additive card's tint (untinted reads warm), weighted by size.
void GlowEmission(const float tint[4], float size, Sum& sum) {
    const float alpha = std::clamp(tint[3], 0.0f, 1.0f);
    const float rgb[3] = { std::max(tint[0], 0.0f), std::max(tint[1], 0.0f), std::max(tint[2], 0.0f) };
    const float peak = std::max({ rgb[0], rgb[1], rgb[2] });
    if (alpha <= 0.0f || peak <= 0.0f) {
        return;
    }
    const float spread = peak - std::min({ rgb[0], rgb[1], rgb[2] });
    const float hue[3] = { spread <= kUntintedLimit ? kUntintedGlow[0] : rgb[0] / peak, spread <= kUntintedLimit ? kUntintedGlow[1] : rgb[1] / peak,
                           spread <= kUntintedLimit ? kUntintedGlow[2] : rgb[2] / peak };
    const float weight = peak * alpha * std::clamp(size / kReferenceCard, 0.25f, 3.0f) * kGlowWeight;
    if (weight >= kMinEmission) {
        sum.Add(hue, weight);
    }
}

struct Card {
    C3Vector lo, hi, mid;
    float tint[4];
    uint32_t first, count;  // skin vertex range
};

// The model's batches drawn at rest that are additive glow cards (glass false) or a fixture's unlit glass (true).
std::vector<Card> Cards(const M2Data& data, const M2SkinProfile& skin, bool glass) {
    std::vector<Card> cards;
    for (uint32_t b = 0; b < skin.m_batches.m_count; ++b) {
        const M2Batch& batch = skin.m_batches[b];
        if (batch.m_materialIndex >= data.m_materials.m_count || batch.m_skinSectionIndex >= skin.m_skinSections.m_count) {
            continue;
        }
        const M2Material& mat = data.m_materials[batch.m_materialIndex];
        const bool additive = mat.m_blendMode == M2BLEND_NO_ALPHA_ADD || mat.m_blendMode == M2BLEND_ADD;
        const bool env = batch.m_textureCoordComboIndex < data.m_textureCoordCombos.m_count && data.m_textureCoordCombos[batch.m_textureCoordComboIndex] > 2;
        // glass: the file's unlit flag on a non-additive batch (M2Shared::Initialize also sets it on mod blends: refused)
        if (env || (glass ? additive || !(mat.m_flags & 1) || mat.m_blendMode >= M2BLEND_MOD : !additive)) {
            continue;
        }
        // shown_at_rest: colour alpha and texture weight not zero throughout
        const bool hasColor = batch.m_colorIndex < data.m_colors.m_count;
        if (hasColor && Peak(data, data.m_colors[batch.m_colorIndex].m_alpha, 1.0f) <= 0.0f) {
            continue;
        }
        if (batch.m_textureWeightComboIndex < data.m_textureWeightCombos.m_count) {
            const uint16_t w = data.m_textureWeightCombos[batch.m_textureWeightComboIndex];
            if (w < data.m_textureWeights.m_count && Peak(data, data.m_textureWeights[w].m_weight, 1.0f) <= 0.0f) {
                continue;
            }
        }
        const M2SkinSection& section = skin.m_skinSections[batch.m_skinSectionIndex];
        Card card{ C3Vector(1e30f, 1e30f, 1e30f), C3Vector(-1e30f, -1e30f, -1e30f), C3Vector(0.0f, 0.0f, 0.0f), { 1.0f, 1.0f, 1.0f, 1.0f }, section.m_vertexStart, 0 };
        for (uint32_t v = section.m_vertexStart; v < section.m_vertexStart + section.m_vertexCount && v < skin.m_vertices.m_count; ++v) {
            const uint16_t g = skin.m_vertices[v];
            if (g >= data.m_vertices.m_count) {
                continue;
            }
            const C3Vector& p = data.m_vertices[g].m_position;
            card.lo = C3Vector(std::min(card.lo.x, p.x), std::min(card.lo.y, p.y), std::min(card.lo.z, p.z));
            card.hi = C3Vector(std::max(card.hi.x, p.x), std::max(card.hi.y, p.y), std::max(card.hi.z, p.z));
            card.mid = card.mid + p;
            ++card.count;
        }
        if (!card.count) {
            continue;
        }
        card.mid = card.mid * (1.0f / card.count);
        // the tint: M2Color's rgb when it is constant (an animated one is left untinted, as benilla's batches)
        if (hasColor) {
            const M2Track<C3Vector>& rgb = data.m_colors[batch.m_colorIndex].m_color;
            if (rgb.m_sequenceKeys.m_count && rgb.m_sequenceKeys[0].m_count == 1) {
                const C3Vector c = FirstKey(data, rgb, C3Vector(1.0f, 1.0f, 1.0f));
                card.tint[0] = c.x;
                card.tint[1] = c.y;
                card.tint[2] = c.z;
            }
        }
        cards.push_back(card);
    }
    return cards;
}

float CardSize(const Card& c) {
    return std::max({ c.hi.x - c.lo.x, c.hi.y - c.lo.y, c.hi.z - c.lo.z }) * 0.5f;
}

// combine + glow_anchor: the light's model-space position and colour x intensity, false when nothing emits.
bool Derive(const CM2Shared& shared, C3Vector& pos, float colour[3]) {
    const M2Data& data = *shared.m_data;
    const M2SkinProfile& skin = *shared.m_skinData;
    const std::string path = Lower(shared.m_fileName);
    const std::string file = path.substr(path.find_last_of('\\') + 1);
    // is_living_swarm, is_light_shaft; ours (wxl-light research 10 §6): props whose glints are not lamps
    if (path.find("\\critter\\") != std::string::npos || file.find("firefl") != std::string::npos ||
        path.find("\\volumetriclights\\") != std::string::npos || Contains(file, { "lightshaft", "lightbeam", "lightray" }) ||
        Contains(file, { "portal", "banner", "pennant", "flag", "miningnode", "waterfall", "steam", "mist", "cauldron", "unlit", "broken" })) {
        return false;
    }
    Sum sum;
    std::vector<C3Vector> emitters;
    for (uint32_t i = 0; i < data.m_particles.m_count; ++i) {
        const M2Particle& p = data.m_particles[i];
        // emits_at_rest
        if (FirstKey<uint8_t>(data, p.m_visibilityTrack, 1) && FirstKey(data, p.m_emissionRateTrack, 1.0f) > 0.0f && EmissionOf(p, sum)) {
            emitters.push_back(p.m_position);
        }
    }
    std::vector<Card> cards = Cards(data, skin, false);
    for (const Card& c : cards) {
        GlowEmission(c.tint, CardSize(c), sum);
    }
    if (sum.weight < kMinEmission) {
        // fixture_glass: a lamp, lantern, candle... whose light is its unlit glass
        if (!Contains(file, { "lamp", "lantern", "candle", "torch", "brazier", "sconce", "chandelier", "candelabra" })) {
            return false;
        }
        sum = Sum();
        emitters.clear();
        cards = Cards(data, skin, true);
        for (const Card& c : cards) {
            GlowEmission(c.tint, CardSize(c), sum);
        }
        if (sum.weight < kMinEmission) {
            return false;
        }
    }
    const float peak = std::max({ sum.rgb[0], sum.rgb[1], sum.rgb[2] });
    if (peak <= 0.0f) {
        return false;
    }
    const float intensity = std::min(sum.weight * kEmissionToIntensity, kMaxIntensity);
    for (int i = 0; i < 3; ++i) {
        colour[i] = sum.rgb[i] / peak * intensity;
    }
    // glow_anchor: the cards' centroids and the emitters within reach of a card vertex
    C3Vector at(0.0f, 0.0f, 0.0f);
    float n = 0.0f;
    for (const C3Vector& e : emitters) {
        bool inReach = cards.empty();
        for (size_t c = 0; c < cards.size() && !inReach; ++c) {
            for (uint32_t v = cards[c].first; v < cards[c].first + cards[c].count && v < skin.m_vertices.m_count && !inReach; ++v) {
                const uint16_t g = skin.m_vertices[v];
                if (g < data.m_vertices.m_count) {
                    const C3Vector d = data.m_vertices[g].m_position - e;
                    inReach = d.x * d.x + d.y * d.y + d.z * d.z <= kEmitterReach * kEmitterReach;
                }
            }
        }
        if (inReach) {
            at = at + e;
            n += 1.0f;
        }
    }
    for (const Card& c : cards) {
        at = at + c.mid;
        n += 1.0f;
    }
    if (n <= 0.0f) {
        return false;
    }
    pos = at * (1.0f / n);
    return true;
}
}  // namespace Emissive

// EMISSIVE: the doodad's derived light into the static list; false while its skin profile is still loading.
bool AddEmissive(const CMapDoodadDef* def) {
    const CM2Model* model = def->m_model;
    const CM2Shared* shared = model ? model->m_shared : nullptr;
    if (!shared || !shared->m_data) {
        return true;    // nothing to read (the doodad's next UpdateBounds tries again)
    }
    for (uint32_t i = 0; i < shared->m_data->m_lights.m_count; ++i) {
        if (shared->m_data->m_lights[i].m_type == M2LIGHT_POINT) {
            return true;    // has_authored_light: the model's own lamp answers for it
        }
    }
    if (!(shared->m_flags & 2) || !shared->m_skinData) {
        return false;   // CM2Shared::FinishLoadingSkinProfile not yet run
    }
    C3Vector local;
    float colour[3];
    if (!Emissive::Derive(*shared, local, colour)) {
        return true;
    }
    const CM2Light* key = reinterpret_cast<const CM2Light*>(reinterpret_cast<const char*>(def) + 1);   // fake, odd
    s_static[key] = Static{ def, local * def->m_mat, { colour[0], colour[1], colour[2] }, -1, 0, false, true };
    s_staticByDoodad[def] = { key };
    return true;
}

// The light's sphere against the view frustum's sides (view space, z forward, ndc = xy / z * scale + offset).
bool InFrustum(const float v[3], float r, const float* proj) {
    const float planes[4][2] = { { proj[0], proj[8] }, { proj[5], proj[9] } };
    for (int axis = 0; axis < 2; ++axis) {
        const float a = planes[axis][0];
        const float b = planes[axis][1];
        // a x + (b - 1) z <= 0 and -a x - (b + 1) z <= 0, each normalised
        if ((a * v[axis] + (b - 1.0f) * v[2]) > r * std::sqrt(a * a + (b - 1.0f) * (b - 1.0f))) {
            return false;
        }
        if ((-a * v[axis] - (b + 1.0f) * v[2]) > r * std::sqrt(a * a + (b + 1.0f) * (b + 1.0f))) {
            return false;
        }
    }
    return v[2] > -r;
}

// The stock M2 point light (CM2Light::SetupLighting: attenuation 0 / 0.7 / 0.03 for every M2 light) falls to kCutoff
// of its peak at this radius.
float Radius(float peak, const float att[3]) {
    const double target = peak / kCutoff - att[0];
    if (!(peak > 0.0f) || target <= 0.0) {
        return 0.0f;
    }
    double r = kMaxRadius;
    if (att[2] > 0.0f) {
        r = 2.0 * target / (att[1] + std::sqrt(static_cast<double>(att[1]) * att[1] + 4.0 * att[2] * target));
    } else if (att[1] > 0.0f) {
        r = target / att[1];
    }
    return static_cast<float>(std::min(r, static_cast<double>(kMaxRadius)));
}

// DrawQuad's quad over the world viewport, cut to the pixel rect [x0, x1) x [y0, y1) (viewport pixels, top-left
// origin): same projection, viewport and uv mapping, so the shader sees the full-viewport uv at every pixel.
void DrawRect(const WorldViewport& vp, float x0, float y0, float x1, float y1) {
    static uint16_t s_indices[4] = { 0, 3, 1, 2 };
    static CGxBatch s_batch = { GxPrim_TriangleStrip, 0, 4, 0, 3 };
    C44Matrix projection;
    GxuXformCreateOrtho(0.0f, vp.w, 0.0f, vp.h, -1.0f, 1.0f, projection);
    GxXformSetProjection(projection);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, 0.0f, 1.0f);
    const float offset = g_theGxDevicePtr->Caps().m_pixelCenterOnEdge ? 0.0f : 0.5f;
    C3Vector pos[4];
    C2Vector tex[4];
    const float px[4] = { x0, x0, x1, x1 }, py[4] = { y1, y0, y0, y1 };   // DrawQuad's corner order
    for (int i = 0; i < 4; ++i) {
        pos[i].x = px[i]; pos[i].y = vp.h - py[i]; pos[i].z = 0.0f;
        tex[i].x = (px[i] + offset) / vp.w; tex[i].y = (py[i] + offset) / vp.h;
    }
    CImVector white(255, 255, 255, 255);
    GxPrimVertexPtr(4, pos, sizeof(C3Vector), nullptr, 0, &white, 0, tex, sizeof(C2Vector), nullptr, 0);
    GxPrimIndexPtr(4, s_indices);
    g_theGxDevicePtr->Draw(&s_batch, 1);
}

// The tiles a light's sphere can reach on screen: the projected corners of its view-space box (conservative); a
// sphere through the near plane covers them all.
void TileSpan(const Light& l, const float* proj, int span[4]) {
    const float r = l.radius, z0 = l.view[2] - r, z1 = l.view[2] + r;
    span[0] = 0; span[1] = 0; span[2] = kGridX - 1; span[3] = kGridY - 1;
    if (z0 < 0.1f) return;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (int axis = 0; axis < 2; ++axis) {
        const float a = proj[axis == 0 ? 0 : 5], b = proj[axis == 0 ? 8 : 9];
        for (float v : { l.view[axis] - r, l.view[axis] + r }) {
            for (float z : { z0, z1 }) {
                const float ndc = a * v / z + b;
                lo[axis] = std::min(lo[axis], ndc);
                hi[axis] = std::max(hi[axis], ndc);
            }
        }
    }
    // ndc x -1..1 left to right, ndc y 1..-1 top to bottom (vf_common NdcToPixel)
    const auto cell = [](float t, int n) { return std::clamp(static_cast<int>(std::floor(t * n)), 0, n - 1); };
    span[0] = cell(lo[0] * 0.5f + 0.5f, kGridX);
    span[2] = cell(hi[0] * 0.5f + 0.5f, kGridX);
    span[1] = cell(0.5f - hi[1] * 0.5f, kGridY);
    span[3] = cell(0.5f - lo[1] * 0.5f, kGridY);
}

// BENILLA light grid: bin, rank each tile's list (benilla ranks a cell by what its centre receives; ours by the
// lights' screen share, the same score the global cap uses), then one draw per tile and pass. A row's neighbouring
// tiles with the same list are drawn as one rect.
template <class Upload>
void DrawGrid(const std::vector<Light>& lights, const WorldViewport& vp, const float* proj, const Upload& upload) {
    static std::vector<uint16_t> s_tiles[kGridX * kGridY];
    for (auto& t : s_tiles) t.clear();
    for (size_t i = 0; i < lights.size(); ++i) {
        int span[4];
        TileSpan(lights[i], proj, span);
        for (int y = span[1]; y <= span[3]; ++y) {
            for (int x = span[0]; x <= span[2]; ++x) {
                s_tiles[y * kGridX + x].push_back(static_cast<uint16_t>(i));
            }
        }
    }
    const auto byScore = [&](uint16_t a, uint16_t b) { return lights[a].score > lights[b].score; };
    for (auto& t : s_tiles) {
        if (static_cast<int>(t.size()) > kPerTile) {
            std::nth_element(t.begin(), t.begin() + kPerTile, t.end(), byScore);
            t.resize(kPerTile);
        }
        std::sort(t.begin(), t.end());   // a canonical order, so equal lists compare equal
    }
    for (int y = 0; y < kGridY; ++y) {
        for (int x = 0; x < kGridX;) {
            const std::vector<uint16_t>& list = s_tiles[y * kGridX + x];
            int run = 1;
            while (x + run < kGridX && s_tiles[y * kGridX + x + run] == list) ++run;
            if (!list.empty()) {
                const float x0 = std::floor(vp.w * x / kGridX), x1 = std::floor(vp.w * (x + run) / kGridX);
                const float y0 = std::floor(vp.h * y / kGridY), y1 = std::floor(vp.h * (y + 1) / kGridY);
                for (size_t first = 0; first < list.size(); first += kLightsPerPass) {
                    upload(list.data() + first, std::min(list.size() - first, static_cast<size_t>(kLightsPerPass)));
                    DrawRect(vp, x0, y0, x1, y1);
                }
            }
            x += run;
        }
    }
}

void Draw(const std::vector<Light>& lights, float maxDist, float intensity) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!Shader(s_shader, "LegionLights")) {
        s_noShader = true;
        return;
    }
    const WorldViewport vp = LiveWorldViewport();
    CGxDevice::SetDepthUser(CGxDevice::DepthUser_Lights, true);
    CGxTex* depth = device->DeviceCopySceneDepth();
    if (!depth || vp.w < 1.0f || vp.h < 1.0f || vp.x < 0.0f || vp.y < 0.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height) {
        return;
    }

    // vf_common c0-c3 as RenderShadowMask sets them; c3.z (the distance clamp) = lightMaxDist.
    const float* proj = reinterpret_cast<const float*>(&device->m_projection);
    const float extent = vp.maxZ - vp.minZ;
    const float common[4][4] = {
        { vp.x, vp.y, vp.w, vp.h },
        { 1.0f, 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { vp.minZ + extent * (1.0f + proj[10]) * 0.5f, extent * proj[14] * 0.5f, maxDist, vp.maxZ },
    };
    // The frame's colour is albedo x the stock light level: divide by it so a lamp adds albedo x lamp, as Legion's
    // light buffer does (night: lamps count for more; noon: about their own colour).
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    const float level = std::max(Luma(info.m_bands.m_ambient) + 0.5f * Luma(info.m_bands.m_diffuse), kMinLightLevel);
    const float fadeSpan = std::max(maxDist * 0.25f, 1.0f);
    const float control[4] = { intensity / level, 1.0f / fadeSpan, maxDist, 0.0f };

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    GxRsSet(GxRs_BlendingMode, GxBlend_ModAdd);     // dst * src + dst
    GxRsSet(GxRs_ColorWrite, 7);
    SetPs(0, &common[0][0], 4);
    SetPs(kControlRegister, control, 1);
    // LIGHTOCC: screen-space shadow rays toward each lamp (LegionLights.hlsl LightVisibility)
    const float shadow[4] = { kShadowDarkness * Var(s_shadows, 1.0f, 0.0f, 1.0f), kShadowMarch, kShadowThickness, kShadowDist };
    SetPs(kShadowRegister, shadow, 1);
    // LIGHTBAKE: c4-c7 view to world (vf_common cViewToWorld, row vectors: the view axes in world space, then the
    // camera), c60 the bake atlas; s1 the atlas
    const float* m = reinterpret_cast<const float*>(&device->m_xforms[GxXform_View].m_mtx[device->m_xforms[GxXform_View].m_level]);
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float viewToWorld[4][4] = {
        { m[0], m[4], m[8], 0.0f }, { m[1], m[5], m[9], 0.0f }, { m[2], m[6], m[10], 0.0f }, { cam.x, cam.y, cam.z, 1.0f },
    };
    SetPs(4, &viewToWorld[0][0], 4);
    const bool baked = s_atlas && Var(s_baked, 1.0f, 0.0f, 1.0f) > 0.0f;
    const float bake[4] = { 1.0f / kAtlasW, s_atlasH ? 1.0f / s_atlasH : 0.0f, static_cast<float>(kFace),
                            baked ? kShadowDarkness * Var(s_baked, 1.0f, 0.0f, 1.0f) : 0.0f };
    SetPs(kBakeRegister, bake, 1);
    Bind(1, baked ? s_atlas : nullptr);
    Bind(0, depth);
    BindNormalBuffer();
    device->RsSet(GxRs_PixelShader, s_shader);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    // one batch of up to kLightsPerPass lights into the shader's registers
    const auto upload = [&](const uint16_t* index, size_t count) {
        float regs[kLightsPerPass * 3][4] = {};
        for (size_t n = 0; n < count; ++n) {
            const Light& l = lights[index[n]];
            float* r = regs[n * 3];
            std::memcpy(r, l.view, sizeof(l.view));
            r[3] = 1.0f / (l.radius * l.radius);
            std::memcpy(r + 4, l.colour, sizeof(l.colour));
            std::memcpy(r + 8, l.att, sizeof(l.att));
            r[7] = l.slot >= 0 ? static_cast<float>(SlotY(l.slot)) : -1.0f;    // LIGHTBAKE: the slot's texel origin
            r[11] = l.slot >= 0 ? static_cast<float>(SlotX(l.slot)) : 0.0f;
        }
        SetPs(kControlRegister + 1, &regs[0][0], kLightsPerPass * 3);
    };
    if (!s_grid || s_grid->m_intValue != 0) {
        DrawGrid(lights, vp, proj, upload);
    } else {
        static std::vector<uint16_t> s_all;
        s_all.resize(lights.size());
        for (size_t i = 0; i < lights.size(); ++i) s_all[i] = static_cast<uint16_t>(i);
        for (size_t first = 0; first < lights.size(); first += kLightsPerPass) {
            upload(s_all.data() + first, std::min(lights.size() - first, static_cast<size_t>(kLightsPerPass)));
            DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
        }
    }
    device->DeviceSceneDepthBound(true);
    EndPasses(s_saved, vp);
}

}  // namespace

void RegisterCVars() {
    const auto reg = [](const char* name, const char* help, const char* value) {
        return CVar::Register(name, help, 1, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    };
    s_mode = reg("lightMode", "Lamps and torches light the world around them per pixel: 0 = off (the stock per-vertex light on ground and buildings), 1 = ground and buildings, 2 = also creatures, characters and grass (default)", "2");
    s_count = reg("maxLightCount", "Most lamps that light the world at once, biggest on screen first, 1..256 (lightGrid: each screen tile draws only its own, up to 32)", "128");
    s_grid = reg("lightGrid", "Lamps are binned into a 16 x 9 grid of screen tiles and each tile lights only from the lamps that reach it (benilla's light grid): 1 = on (default), 0 = every lamp over the whole screen (16 a pass)", "1");
    s_dist = reg("maxLightDist", "Distance in yards out to which lamps light the world (fades out over the last quarter), 20..2048", "200");
    s_normalBuffer = reg("gxNormalBuffer", "The world's models, buildings and ground also write their smooth normals, which the lamps (lightMode), the mountains' shadow (shadowMask) and SSAO light by: 1 = on (default), 0 = rebuild them from the depth (faceted)", "1");
    s_intensity = reg("lightIntensity", "Brightness of the lamp light on the world, 0..4", "1");
    s_baked = reg("lightBakedShadows", "Walls, ground and buildings between a fixed lamp and a surface block its light, worked out once per lamp (also off screen): 0 = off, 1 = full (default)", "1");
    s_atlasSize = reg("lightBakedShadowAtlas", "Texels high of the baked lamp shadow store, 64..4096 (a power of two; 16 rows of 10 lamps, 64 KB a row): 512 = 320 lamps, 2 MB (default)", "512");
    s_emissive = reg("lightEmissive", "Doodads that draw a flame or a glow but carry no light of their own (braziers, candles, glowing lanterns) light the world around them, from the colour of that glow: 1 = on (default), 0 = off", "1");
    s_shadows = reg("lightShadows", "Things standing between a lamp and a surface block its light (screen-space shadow rays): 0 = off, 1 = full (default), between = lighter shadows", "1");
}

void AddDoodad(const CMapDoodadDef* def) {
    RemoveDoodad(def);
    CM2Model* model = def->m_model;
    const M2Data* data = model && model->m_shared ? model->m_shared->m_data : nullptr;
    if (!data) {
        return;
    }
    std::vector<const CM2Light*> keys;
    for (uint32_t i = 0; model->m_lights && i < data->m_lights.m_count; i++) {
        const M2Light& light = data->m_lights[i];
        const float intensity = FirstKey(*data, light.m_diffuseIntensity, 1.0f);
        const C3Vector colour = FirstKey(*data, light.m_diffuseColor, C3Vector(0.0f, 0.0f, 0.0f));
        if (light.m_type != M2LIGHT_POINT || !FirstKey<uint8_t>(*data, light.m_visibility, 1) || !(intensity > 0.0f)) {
            continue;
        }
        // ponytail: the bone's rest pose taken as identity (lamps hang from static or barely swinging bones)
        const CM2Light* key = &model->m_lights[i].m_light;
        Static& s = s_static[key];
        s = Static{ def, light.m_position * def->m_mat, { colour.x * intensity, colour.y * intensity, colour.z * intensity }, -1, 0, false, false };
        keys.push_back(key);
    }
    if (!keys.empty()) {
        s_staticByDoodad[def] = std::move(keys);
    } else if (!AddEmissive(def)) {
        s_pending.insert(def);  // EMISSIVE: no authored light; derived once its skin profile is in
    }
}

void RemoveDoodad(const CMapDoodadDef* def) {
    s_pending.erase(def);
    auto it = s_staticByDoodad.find(def);
    if (it == s_staticByDoodad.end()) {
        return;
    }
    for (const CM2Light* key : it->second) {
        auto entry = s_static.find(key);
        if (entry != s_static.end()) {
            FreeSlot(entry->second.slot);
            s_static.erase(entry);
        }
        if (s_job.key == key) {
            FreeSlot(s_job.slot);
            s_job.key = nullptr;
        }
        s_seen.erase(key);
    }
    s_staticByDoodad.erase(it);
}

bool LightsWorldGeometry() {
    const int mode = s_mode ? s_mode->m_intValue : 0;
    return (mode == 1 || mode == 2) && !s_noShader && !g_m2ShaderOwnedLightScene && Var(s_intensity, 1.0f, 0.0f, 4.0f) > 0.0f;
}

void OnWorld(bool solidDone) {
    CGxDevice::s_normalBuffer = !s_normalBuffer || s_normalBuffer->m_intValue != 0;
    const int mode = s_mode ? s_mode->m_intValue : 0;
    if (!solidDone) {
        CGxDevice::SetDepthUser(CGxDevice::DepthUser_Lights, false);    // set again by Draw
        for (auto it = s_pending.begin(); it != s_pending.end();) {     // EMISSIVE: skins that have arrived
            it = AddEmissive(*it) ? s_pending.erase(it) : std::next(it);
        }
    }
    CM2Scene* scene = World::s_m2Scene;
    CGxDevice* device = g_theGxDevicePtr;
    if (!solidDone) {
        // lightMode 2: the models' stock per-vertex point lights give way to the pass (CM2Model::SetupLighting)
        g_m2PixelLitScene = mode == 2 && LightsWorldGeometry() ? scene : nullptr;
    }
    // POINTLIGHT: the forward lightDynamic (D3D11/Vulkan shaderLighting) owns the lights: one per-pixel light is enough
    if (mode < 1 || mode > 2 || (mode == 2) != solidDone || !scene || !scene->m_lightGrid || !device || g_m2ShaderOwnedLightScene) {
        return;
    }
    const float maxDist = Var(s_dist, 200.0f, 20.0f, 2048.0f);
    const float intensity = Var(s_intensity, 1.0f, 0.0f, 4.0f);
    const int maxCount = static_cast<int>(Var(s_count, 64.0f, 1.0f, static_cast<float>(kMaxLights)));
    if (intensity <= 0.0f) {
        return;
    }

    // The Gx view top is camera relative (CWorldScene::Update); rows are view axes (row vectors, as vf_common).
    const CGxMatrixStack& viewStack = device->m_xforms[GxXform_View];
    const float* m = reinterpret_cast<const float*>(&viewStack.m_mtx[viewStack.m_level]);
    const C3Vector& cam = CWorldScene::s_activeWorldView;

    const float* proj = reinterpret_cast<const float*>(&device->m_projection);
    static std::vector<Light> s_lights;
    s_lights.clear();
    ++s_walk;
    ++s_frame;
    const auto add = [&](const C3Vector& pos, const float c[3], const float att[3], const CM2Light* key) {
        Light l;
        const float rel[3] = { pos.x - cam.x, pos.y - cam.y, pos.z - cam.z };
        std::memcpy(l.att, att, sizeof(l.att));
        float peak = 0.0f;
        for (int i = 0; i < 3; ++i) {
            l.view[i] = rel[0] * m[i] + rel[1] * m[4 + i] + rel[2] * m[8 + i];
            l.colour[i] = std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, kMaxColour) : 0.0f;
            peak = std::max(peak, l.colour[i]);
        }
        l.radius = Radius(peak, l.att);
        l.distance = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        if (l.radius < 0.5f || !std::isfinite(l.distance) || l.distance > maxDist + l.radius || !InFrustum(l.view, l.radius, proj)) {
            return;     // too weak, out of reach, or its sphere wholly outside the view
        }
        l.score = l.radius / std::max(l.distance, 1.0f);   // about the share of the view its light covers
        l.key = key;
        l.slot = -1;
        s_lights.push_back(l);
    };
    for (int cell = 0; cell < 64 * 64; ++cell) {
        for (const CM2Light* native = scene->m_lightGrid[cell]; native; native = native->m_lightNext) {
            if (native->m_type != M2LIGHT_POINT) {
                continue;
            }
            if (s_static.count(native)) {
                continue;   // LAMPSON: a placed lamp, lit below from the static list
            }
            const bool fresh = !native->m_scene || native->m_stamp == scene->m_frameStamp;
            Seen& seen = s_seen.try_emplace(native, Seen{ native->m_pos, 0, 0, 0, 0 }).first->second;
            seen.walk = s_walk;
            if (fresh) {
                const C3Vector d = native->m_pos - seen.pos;
                seen.moved |= seen.refreshes && d.x * d.x + d.y * d.y + d.z * d.z > 0.0025f;
                seen.pos = native->m_pos;
                seen.visible = native->m_visible != 0;
                seen.refreshes = std::min<uint32_t>(seen.refreshes + 1, 2);
            }
            // stale: only a light seen on at least twice, still where it was
            const bool on = fresh ? seen.visible : seen.visible && !seen.moved && seen.refreshes >= 2u &&
                            native->m_pos.x == seen.pos.x && native->m_pos.y == seen.pos.y && native->m_pos.z == seen.pos.z;
            if (!on) {
                continue;
            }
            const float c[3] = { native->m_dirColor.x, native->m_dirColor.y, native->m_dirColor.z };
            const float att[3] = { native->m_constantAttenuation, native->m_linearAttenuation, native->m_quadraticAttenuation };
            add(native->m_pos, c, att, nullptr);
        }
    }
    const bool emissiveOff = s_emissive && s_emissive->m_intValue == 0;
    // LAMPSON: every placed doodad's lamp, always, at its static colour; the live light only moves it while animated
    for (const auto& entry : s_static) {
        const CM2Light* key = entry.first;
        if (entry.second.emissive && emissiveOff) {
            continue;
        }
        const bool live = !entry.second.emissive && key->m_scene == scene && key->m_stamp == scene->m_frameStamp && key->m_visible;
        const float att[3] = { 0.0f, 0.7f, 0.03f };     // CM2Light's, every M2 light
        add(live ? key->m_pos : entry.second.pos, entry.second.colour, att, key);
    }
    if ((s_walk & 63) == 0) {   // lights no longer in the grid (their models gone): drop
        for (auto it = s_seen.begin(); it != s_seen.end();) {
            it = it->second.walk == s_walk ? std::next(it) : s_seen.erase(it);
        }
    }
    if (s_lights.empty()) {
        return;
    }
    if (static_cast<int>(s_lights.size()) > maxCount) {
        std::nth_element(s_lights.begin(), s_lights.begin() + maxCount, s_lights.end(),
                         [](const Light& a, const Light& b) { return a.score > b.score; });
        // LAMPSON: the kept lights near the cut fade in over a quarter of its score, so a lamp crossing the cap as
        // the camera turns dims through it instead of switching (s_lights[maxCount] is the best one dropped)
        const float cut = s_lights[maxCount].score;
        s_lights.resize(maxCount);
        for (Light& l : s_lights) {
            const float w = std::clamp((l.score - cut) / std::max(cut * 0.25f, 1e-6f), 0.0f, 1.0f);
            for (float& c : l.colour) {
                c *= w;
            }
        }
    }
    // LIGHTBAKE: the selected static lamps take their baked slots; the best unbaked one bakes on (kBakeMs a frame)
    if (Var(s_baked, 1.0f, 0.0f, 1.0f) > 0.0f && PrepareAtlas()) {
        const Light* next = nullptr;
        for (Light& l : s_lights) {
            auto it = l.key ? s_static.find(l.key) : s_static.end();
            if (it == s_static.end()) {
                continue;
            }
            it->second.used = s_frame;
            l.slot = it->second.slot;
            if (l.slot < 0 && !it->second.noBake && l.key != s_job.key && (!next || l.score > next->score)) {
                next = &l;
            }
        }
        if (!s_job.key && next) {
            const int32_t slot = AllocSlot();
            if (slot >= 0) {
                s_slots[slot] = next->key;
                s_job.key = next->key;
                s_job.slot = slot;
                s_job.next = 0;
                s_job.shut = 0;
                s_job.radius = next->radius;
            }
        }
        if (s_job.key) {
            RunBake();
        }
    } else if (s_atlas) {
        DropAtlas();
    }
    Draw(s_lights, maxDist, intensity);
}

}  // namespace LegionLights
