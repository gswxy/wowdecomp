#include "WoW/Source/Ui/WorldLighting.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/GxLighting.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2ModelLight.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLoad.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"

// CUSTOM (Badgermilk0 port of AwesomeLighting.cpp): the CVars and what the passes need of the zone, and the shader
// half: per-pixel relighting, dynamic point lights (M2 light grid, WMO MOLT lights), specular and sky ambient, which
// the lit world shaders read from g_gxLightingGpu (GxLighting.h). Badgermilk's translator rewrote the world shaders at
// run time; here the rewrite is offline (shader-xlate sm3hlsl.py for D3D11, Tools/VulkanShaders for Vulkan), so the
// lit shaders come with custom.MPQ. D3D9 keeps the stock shaders (g_gxLighting is null there).

namespace {

// UNITLIGHTS: units (and the weapons, helms and mounts attached to them) keep the stock per-vertex point lights under
// lightReplaceStock. The dynamic per-pixel lights light the doodads and WMOs but never reached the units' draws (in game
// 2026-10-25: lightReplaceStock 0 lit the NPCs at a campfire, 1 left them dark on the side facing it); until that is
// found, the units keep the stock lights the dynamic ones were meant to replace. A unit's world entity carries
// m_staticFlags 0x8000 (CMap::ObjectCreate flag 0x10, CGObject_C for TYPE_UNIT; the character-ambient boost).
bool KeepStockPointLights(const CM2Model* model) {
    while (model->m_attachParent) {
        model = model->m_attachParent;
    }

    return model->m_lightingCallback == CMapStaticEntity::ModelLightingCallback && model->m_lightingArg
        && (static_cast<const CMapStaticEntity*>(model->m_lightingArg)->m_staticFlags & 0x8000);
}

struct LightingCVars {
    CVar* hdr;
    CVar* exposure;
    CVar* tonemap;
    CVar* bloom;
    CVar* bloomThreshold;
    CVar* ssao;
    CVar* ssaoRadius;
    CVar* ssaoDistance;
    CVar* fogScattering;
    CVar* debug;
    // the shader half
    CVar* master;
    CVar* perPixel;
    CVar* dynamic;
    CVar* dynamicIntensity;
    CVar* dynamicM2;
    CVar* dynamicWmo;
    CVar* dynamicDistance;
    CVar* dynamicMinRadius;
    CVar* replaceStock;
    CVar* wmoInsideOnly;
    CVar* flicker;
    CVar* wrap;
    CVar* specular;
    CVar* specularM2;
    CVar* specularWmo;
    CVar* skyAmbient;
    CVar* pbr;
    CVar* roughnessBias;
    CVar* metalPlate;
    CVar* metalMail;
    CVar* wmoNormalMaps;
    CVar* wmoNormalStrength;
};

LightingCVars s_cv;
bool s_inWorld = false;
GxLightingParams s_params;

CVar* Reg(const char* name, const char* help, const char* value) {
    return CVar::Register(name, help, 0x10, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

float Positive(CVar* cv) {
    return cv && cv->m_floatValue > 0.0f ? cv->m_floatValue : 0.0f;
}

C3Vector ColorOf(const CImVector& c) {
    return C3Vector(c.r * (1.0f / 255.0f), c.g * (1.0f / 255.0f), c.b * (1.0f / 255.0f));
}

C3Vector Lerp(const C3Vector& a, const C3Vector& b, float t) {
    return C3Vector(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

uint32_t Features() {
    if (!s_cv.hdr) {
        return 0;
    }

    uint32_t features = 0;

    // bloom rides the hdr pass; without hdr the tonemap is the stock clamp
    if (s_cv.hdr->m_intValue || Positive(s_cv.bloom) > 0.0f) {
        features |= GxLighting_Hdr;
    }

    if (Positive(s_cv.bloom) > 0.0f) {
        features |= GxLighting_Bloom;
    }

    if (Positive(s_cv.ssao) > 0.0f) {
        features |= GxLighting_Ssao;
    }

    // vfogaudit: the stock fog it tints is pushed out while vfog draws, and the fitted fog would get the sun twice
    if (Positive(s_cv.fogScattering) > 0.0f && !VolumetricFog::Active()) {
        features |= GxLighting_Scattering;
    }

    // shaderLighting 0: every light* feature off, whatever its own CVar says
    if (s_cv.master->m_intValue <= 0) {
        return features;
    }

    if (s_cv.perPixel->m_intValue > 0) {
        features |= GxLighting_PerPixel;
    }

    if (s_cv.dynamic->m_intValue > 0) {
        features |= GxLighting_Dynamic;
    }

    if (s_cv.specular->m_intValue > 0) {
        features |= GxLighting_Specular;
    }

    if (s_cv.pbr->m_intValue > 0) {
        features |= GxLighting_Pbr;   // ARMORMETAL: no forced sky ambient (its blue-grey cast); lightSkyAmbient alone
    }

    if (Positive(s_cv.skyAmbient) > 0.0f) {
        features |= GxLighting_Hemisphere;
    }

    return features;
}

void BuildParams(GxLightingParams& p, uint32_t features, const C3Vector& camera) {
    p.m_features = features;
    p.m_exposure = s_cv.exposure->m_floatValue > 0.0f ? s_cv.exposure->m_floatValue : 1.0f;
    p.m_tonemap = !s_cv.hdr->m_intValue || s_cv.tonemap->m_intValue < 0 ? 0u : static_cast<uint32_t>(s_cv.tonemap->m_intValue);
    p.m_bloomIntensity = Positive(s_cv.bloom);
    p.m_bloomThreshold = s_cv.bloomThreshold->m_floatValue > 0.01f ? s_cv.bloomThreshold->m_floatValue : 0.01f;
    p.m_ssaoIntensity = Positive(s_cv.ssao);
    p.m_ssaoRadius = s_cv.ssaoRadius->m_floatValue > 0.1f ? s_cv.ssaoRadius->m_floatValue : 0.1f;
    p.m_ssaoDistance = s_cv.ssaoDistance->m_floatValue > 10.0f ? s_cv.ssaoDistance->m_floatValue : 10.0f;
    p.m_scattering = Positive(s_cv.fogScattering);
    p.m_lightWrap = std::min(Positive(s_cv.wrap), 1.0f);
    p.m_dynamicIntensity = std::min(Positive(s_cv.dynamicIntensity), 8.0f);
    p.m_specularM2 = std::min(Positive(s_cv.specularM2), 4.0f);
    p.m_specularWmo = std::min(Positive(s_cv.specularWmo), 4.0f);
    p.m_hemisphere = std::min(Positive(s_cv.skyAmbient), 1.0f);
    p.m_roughnessBias = std::clamp(s_cv.roughnessBias->m_floatValue, -1.0f, 1.0f);
    p.m_metalPlate = std::clamp(s_cv.metalPlate->m_floatValue, 0.0f, 1.0f);
    p.m_metalMail = std::clamp(s_cv.metalMail->m_floatValue, 0.0f, 1.0f);
    p.m_wmoNormal = s_cv.wmoNormalMaps->m_intValue > 0 ? std::min(Positive(s_cv.wmoNormalStrength), 2.0f) : 0.0f;
    p.m_cameraPosition = camera;
    p.m_time = static_cast<float>(OsGetAsyncTimeMs() % 3600000u) * 0.001f;
    p.m_debug = s_cv.debug->m_intValue < 0 ? 0u : static_cast<uint32_t>(s_cv.debug->m_intValue);

    // the zone's fog and sun this moment (Light.dbc through DayNight)
    DayNight::DNInfo* info = DayNight::GetInfo();

    if (!info) {
        return;
    }

    float interior = info->m_interiorFogBlend < 0.0f ? 0.0f : (info->m_interiorFogBlend > 1.0f ? 1.0f : info->m_interiorFogBlend);
    p.m_fogColor = Lerp(ColorOf(info->m_fog.m_color), ColorOf(info->m_fogInterior.m_color), interior);
    p.m_fogStart = info->m_fog.m_start + (info->m_fogInterior.m_start - info->m_fog.m_start) * interior;
    p.m_fogEnd = info->m_fog.m_end + (info->m_fogInterior.m_end - info->m_fog.m_end) * interior;
    p.m_fogDensity = info->m_fog.m_density > 0.0f ? info->m_fog.m_density : 1.0f;
    p.m_sunColor = ColorOf(info->m_bands.m_diffuse);
    // the sky's top and middle; the ground: the fog lit by the sun
    p.m_skyColor = Lerp(ColorOf(info->m_bands.m_skyColors[0]), ColorOf(info->m_bands.m_skyColors[1]), 0.5f);
    p.m_groundColor = Lerp(p.m_fogColor, p.m_sunColor, 0.6f);
    p.m_sunDirection = info->m_light0.m_dir;
}


// ---- the shader half: lights (Badgermilk0 AwesomeLighting.cpp, fires-as-lights left out) ----

// a gathered light and where it came from (the name only lives this frame: the dump reads it before the model can go)
struct Gathered {
    GxLightingLight m_light;
    bool m_m2;
    bool m_static;        // a model never animated: its light read from the file, not the animation
    const char* m_name;
    int32_t m_index;
    float m_distance;
    float m_score;
    int32_t m_rank;       // -1: the cap dropped it
};

std::vector<Gathered> s_gathered;
std::vector<GxLightingLight> s_lights;
uint32_t s_wmoDeduped = 0;
bool s_dumpPending = false;

float Decode(float c) {
    return c > 0.0f ? powf(c, 2.2f) : 0.0f;
}

C3Vector Transform(const C3Vector& p, const C44Matrix& m) {
    return C3Vector(p.x * m.a0 + p.y * m.b0 + p.z * m.c0 + m.d0, p.x * m.a1 + p.y * m.b1 + p.z * m.c1 + m.d1, p.x * m.a2 + p.y * m.b2 + p.z * m.c2 + m.d2);
}

float DistanceSquared(const C3Vector& a, const C3Vector& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Luminance(const C3Vector& c) {
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

// a warm light (a flame) flickers; a cold or white one (magic, a lamp) does not
float FlickerFor(const C3Vector& linear) {
    return linear.x > linear.y * 1.1f && linear.y > linear.z ? std::min(Positive(s_cv.flicker), 1.0f) : 0.0f;
}

// flame: a model's light (a torch), which may flicker; a WMO's lights are its rooms' steady light
Gathered* AddLight(const C3Vector& position, const C3Vector& gamma, float scale, float start, float radius, const C3Vector& camera, float reach, bool flame) {
    if (radius <= 0.1f || scale <= 0.0f || DistanceSquared(position, camera) > (reach + radius) * (reach + radius)) {
        return nullptr;
    }

    GxLightingLight light;
    light.m_position = position;
    light.m_radius = radius;
    light.m_start = start < radius ? (start > 0.0f ? start : 0.0f) : radius * 0.25f;
    // a model's light can be animated far past 1; a few times white is as bright as a light gets here
    light.m_color = C3Vector(fminf(Decode(gamma.x), 4.0f) * scale, fminf(Decode(gamma.y), 4.0f) * scale, fminf(Decode(gamma.z), 4.0f) * scale);
    // a large light flickers less: a room lit as a whole should not pulse
    light.m_flicker = flame ? FlickerFor(light.m_color) * fminf(1.0f, 8.0f / radius) : 0.0f;
    light.m_specular = 1.0f;

    // UCLIGHTS: fade out over the last quarter of the gather's reach instead of switching off at its edge
    float edge = std::clamp(((reach + radius) - sqrtf(DistanceSquared(position, camera))) / std::max(reach * 0.25f, 1.0f), 0.0f, 1.0f);
    light.m_color = light.m_color * (edge * edge * (3.0f - 2.0f * edge));

    if (light.m_color.x + light.m_color.y + light.m_color.z < 0.003f) {
        return nullptr;
    }

    // what it gives the camera's surroundings: all of it when the camera is inside its reach, none at the gather's edge
    float d = sqrtf(DistanceSquared(position, camera));
    float falloff = std::clamp(1.0f - std::max(d - radius, 0.0f) / reach, 0.0f, 1.0f);
    s_gathered.push_back(Gathered { light, flame, false, nullptr, -1, d, Luminance(light.m_color) * falloff, -1 });
    return &s_gathered.back();
}

template <class T>
bool FirstKey(const M2Track<T>& track, T& out) {
    if (!track.m_sequenceKeys.m_count || !track.m_sequenceKeys[0].m_count) {
        return false;
    }

    out = track.m_sequenceKeys[0][0];
    return true;
}

// the point lights of the models in the scene. Not CM2Scene's light grid: a model off screen is not animated, and the
// stock CM2Scene::SelectLights unlinks the lights of a model not animated this frame, so a torch behind the camera would
// drop out although it still lights what is on screen. Every loaded model is in m_modelList; one off screen keeps the
// place, colour and attenuation its lights had when it was last animated. A model never animated (loaded off screen)
// has none yet: its light is read from the file (first key, bone at rest) instead of animating the model
void GatherM2Lights(CM2Scene* scene, const C3Vector& camera, float reach) {
    float scale = Positive(s_cv.dynamicM2);
    float minRadius = std::clamp(s_cv.dynamicMinRadius->m_floatValue, 0.0f, 40.0f);

    if (!scene || scale <= 0.0f) {
        return;
    }

    // ponytail: walks every loaded model (a pointer test each, most have no lights; AddLight tests the reach), not by its
    // world transform, which an attached model (a held torch) keeps relative to its parent; a grid of lit models if it shows
    for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
        if (!model->m_lights || !model->m_shared || !model->m_shared->m_data) {
            continue;
        }

        M2Data* data = model->m_shared->m_data;

        for (uint32_t i = 0; i < data->m_lights.m_count; i++) {
            const M2ModelLight& owner = model->m_lights[i];
            const CM2Light& light = owner.m_light;

            if (!owner.m_enabled || light.m_type != M2LIGHT_POINT || !light.m_scene) {
                continue;
            }

            C3Vector position = light.m_pos;
            C3Vector color = light.m_dirColor;
            float end = owner.m_attenuationEndTrack.m_currentValue;
            float start = owner.m_attenuationStartTrack.m_currentValue;
            bool fromFile = light.m_stamp == 0;

            if (fromFile) {
                // never animated: an attached model's transform is its parent's to give, so it waits for its first frame
                const M2Light& file = data->m_lights[i];
                uint8_t visible = 1;
                float intensity = 1.0f;
                color = C3Vector(0.0f, 0.0f, 0.0f);
                FirstKey(file.m_visibility, visible);
                FirstKey(file.m_diffuseIntensity, intensity);

                if (model->m_attachParent || !visible || !FirstKey(file.m_diffuseColor, color)) {
                    continue;
                }

                color = color * intensity;
                position = Transform(file.m_position, model->m_worldTransform);
                end = start = 0.0f;
                FirstKey(file.m_attenuationEnd, end);
                FirstKey(file.m_attenuationStart, start);
            } else if (!owner.m_visibilityTrack.m_currentValue) {
                continue;
            }

            if (!(end > 0.25f && end < 80.0f)) {
                end = 8.0f;
                start = 1.0f;
            }

            // the stock falloff 1/(0.7d + 0.03d²) still gives a tenth at 10 yd: a 1-3 yd end would barely reach a wall
            end = std::max(end, minRadius);

            if (Gathered* g = AddLight(position, color, scale, start, end, camera, reach, true)) {
                g->m_static = fromFile;
                g->m_name = model->m_shared->m_fileNameWithoutPath ? model->m_shared->m_fileNameWithoutPath : model->m_shared->m_fileName;
                g->m_index = static_cast<int32_t>(i);
            }
        }
    }
}

// the MOLT lights of the loaded groups of the placed WMOs, which the stock client only baked into vertex colours. A
// light within 1.5 yd of a model's light is that model's (the torch the WMO baked): the model's, animated, is kept
void GatherWmoLights(const C3Vector& camera, float reach) {
    float scale = Positive(s_cv.dynamicWmo);
    size_t m2End = s_gathered.size();
    std::vector<uint8_t> done;

    if (scale <= 0.0f) {
        return;
    }

    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        CMapObj* owner = def->m_owner;

        if ((def->m_flags & 0x20) || !owner || !owner->m_lightList || owner->m_ligtsCount <= 0) {
            continue;
        }

        done.assign(owner->m_ligtsCount, 0);

        for (CMapBaseObjLink* link = def->m_mapObjDefGroupLinkList.Head(); link; link = def->m_mapObjDefGroupLinkList.Next(link)) {
            CMapObjDefGroup* defGroup = static_cast<CMapObjDefGroup*>(link->m_owner);
            CMapObjGroup* group = owner->GetGroup(defGroup->m_groupNum, 0);

            if (!group || !group->m_lightRefList) {
                continue;
            }

            // the group within reach. UCLIGHTS: an interior one no longer only with the camera inside its box: Undercity is
            // all interior groups, so whole rooms of lamps switched on and off as the camera crossed from one box into the
            // next (lightDynamicWmoInsideOnly 1 brings that back). A room's lights may now reach through a thin wall.
            // ponytail: no shadowing of WMO lights through walls; add a portal/collision test if rooms visibly leak
            const bool insideOnly = s_cv.wmoInsideOnly && s_cv.wmoInsideOnly->m_intValue != 0;
            const CAaBox& box = defGroup->m_bbox;
            C3Vector nearest(std::clamp(camera.x, box.b.x, box.t.x), std::clamp(camera.y, box.b.y, box.t.y), std::clamp(camera.z, box.b.z, box.t.z));
            float d2 = DistanceSquared(nearest, camera);

            if (d2 > reach * reach || (insideOnly && (defGroup->m_flags & MAPOBJ_FLAG_INTERIOR) && d2 > 0.0f)) {
                continue;
            }

            for (int32_t r = 0; r < group->m_lightRefListCount; r++) {
                uint16_t i = group->m_lightRefList[r];

                if (i >= owner->m_ligtsCount || done[i]) {
                    continue;
                }

                done[i] = 1;
                const SMOLight& molt = owner->m_lightList[i];

                // omni and spot lights (a spot lights all round here); directional and ambient ones are the stock lighting
                if (molt.m_type > 1 || molt.m_attenEnd <= 0.25f) {
                    continue;
                }

                C3Vector position = Transform(molt.m_position, def->m_mat);
                bool twin = false;

                for (size_t m = 0; m < m2End && !twin; m++) {
                    twin = DistanceSquared(s_gathered[m].m_light.m_position, position) < 1.5f * 1.5f;
                }

                if (twin) {
                    s_wmoDeduped++;
                    continue;
                }

                float intensity = molt.m_intensity > 0.0f ? molt.m_intensity : 1.0f;

                if (Gathered* g = AddLight(position, ColorOf(molt.m_color), scale * intensity, molt.m_attenStart, molt.m_attenEnd, camera, reach, false)) {
                    g->m_name = owner->m_wmoName;
                    g->m_index = i;
                }
            }
        }
    }
}

// the lights the GPU gets: the brightest by what they give the camera's surroundings, a model's (a torch) first; the
// WMOs keep up to a quarter of the places, so a city's hundreds of room lights cannot crowd out the torches
void SelectLights() {
    std::vector<Gathered*> m2;
    std::vector<Gathered*> wmo;

    for (Gathered& g : s_gathered) {
        (g.m_m2 ? m2 : wmo).push_back(&g);
    }

    auto brighter = [](const Gathered* a, const Gathered* b) { return a->m_score > b->m_score; };
    std::sort(m2.begin(), m2.end(), brighter);
    std::sort(wmo.begin(), wmo.end(), brighter);
    size_t wmoReserve = std::min<size_t>(wmo.size(), kGxLightingMaxLights / 4);
    size_t m2Take = std::min(m2.size(), kGxLightingMaxLights - wmoReserve);
    size_t wmoTake = std::min(wmo.size(), kGxLightingMaxLights - m2Take);
    int32_t rank = 0;

    // UCLIGHTS: the kept lights near the cap fade over a quarter of the best dropped one's score, so a light crossing the
    // cap as the camera moves dims through it instead of popping (as LegionLights' maxLightCount cut)
    auto take = [&](std::vector<Gathered*>& list, size_t n) {
        float cut = n < list.size() ? list[n]->m_score : 0.0f;
        for (size_t i = 0; i < n; i++) {
            Gathered* g = list[i];
            g->m_rank = rank++;
            if (cut > 0.0f) {
                float w = std::clamp((g->m_score - cut) / (cut * 0.25f), 0.0f, 1.0f);
                g->m_light.m_color = g->m_light.m_color * w;
            }
        }
    };
    take(m2, m2Take);
    take(wmo, wmoTake);

    s_lights.clear();

    for (const Gathered& g : s_gathered) {
        if (g.m_rank >= 0) {
            s_lights.push_back(g.m_light);
        }
    }
}

void DumpLights(CM2Scene* scene, const C3Vector& camera) {
    uint32_t kept = 0;

    for (const Gathered& g : s_gathered) {
        kept += g.m_rank >= 0;
    }

    ConsoleWriteA("lightingDump: %u gathered, %u kept (cap %u), %u WMO lights deduped against model lights", DEFAULT_COLOR, static_cast<uint32_t>(s_gathered.size()), kept, kGxLightingMaxLights, s_wmoDeduped);

    for (const Gathered& g : s_gathered) {
        const GxLightingLight& l = g.m_light;
        ConsoleWriteA("%s %s %s#%d at (%.1f %.1f %.1f) r %.1f col (%.2f %.2f %.2f) dist %.1f score %.3f %s", g.m_rank >= 0 ? DEFAULT_COLOR : WARNING_COLOR,
            g.m_m2 ? "M2 " : "WMO", g.m_static ? "(file)" : "", g.m_name ? g.m_name : "?", g.m_index, l.m_position.x, l.m_position.y, l.m_position.z,
            l.m_radius, l.m_color.x, l.m_color.y, l.m_color.z, g.m_distance, g.m_score, g.m_rank >= 0 ? "kept" : "DROPPED by cap");
    }

    // the stock side: every model light within 30 yd, whether the stock grid has it (visible) and how stale it is
    if (!scene) {
        return;
    }

    ConsoleWriteA("stock model lights within 30 yd (frame %u):", DEFAULT_COLOR, scene->m_frameStamp);

    for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
        if (!model->m_lights || !model->m_shared || !model->m_shared->m_data) {
            continue;
        }

        for (uint32_t i = 0; i < model->m_shared->m_data->m_lights.m_count; i++) {
            const CM2Light& light = model->m_lights[i].m_light;

            if (light.m_type != M2LIGHT_POINT || !light.m_stamp || DistanceSquared(light.m_pos, camera) > 30.0f * 30.0f) {
                continue;
            }

            ConsoleWriteA("  %s#%u at (%.1f %.1f %.1f) col (%.2f %.2f %.2f) %s, stamp age %u", light.m_visible ? DEFAULT_COLOR : WARNING_COLOR,
                model->m_shared->m_fileNameWithoutPath ? model->m_shared->m_fileNameWithoutPath : model->m_shared->m_fileName, i,
                light.m_pos.x, light.m_pos.y, light.m_pos.z, light.m_dirColor.x, light.m_dirColor.y, light.m_dirColor.z,
                light.m_visible ? "in grid" : "NOT in grid", scene->m_frameStamp - light.m_stamp);
        }
    }
}

int32_t CCommand_LightingDump(const char* command, const char* arguments) {
    if (!g_gxLighting) {
        ConsoleWrite("lightingDump: no shader lighting on this device (D3D11/Vulkan only)", WARNING_COLOR);
    } else if (s_cv.master->m_intValue <= 0 || s_cv.dynamic->m_intValue <= 0) {
        ConsoleWrite("lightingDump: shaderLighting and lightDynamic must be on", WARNING_COLOR);
    } else {
        s_dumpPending = true;
    }

    return 1;
}

// a light kept from frame to frame, matched by position (world space): each fades in over 0.3 s and out over 0.6 s
// instead of switching
struct TrackedLight {
    GxLightingLight m_light;
    float m_weight;
    bool m_seen;
};

std::vector<TrackedLight> s_tracked;
uint32_t s_trackedTime = 0;

void SmoothLights() {
    uint32_t now = OsGetAsyncTimeMs();
    float dt = s_trackedTime ? std::min(static_cast<float>(now - s_trackedTime) * 0.001f, 0.25f) : 0.0f;
    s_trackedTime = now;

    for (TrackedLight& tracked : s_tracked) {
        tracked.m_seen = false;
    }

    // a found light's colour, radius and place eased toward over about a quarter second
    float ease = 1.0f - expf(-dt / 0.25f);

    for (const GxLightingLight& light : s_lights) {
        TrackedLight* match = nullptr;
        float best = 3.0f * 3.0f;

        for (TrackedLight& tracked : s_tracked) {
            float d = DistanceSquared(tracked.m_light.m_position, light.m_position);

            if (!tracked.m_seen && d < best) {
                best = d;
                match = &tracked;
            }
        }

        if (match) {
            GxLightingLight& kept = match->m_light;
            kept.m_position = kept.m_position + (light.m_position - kept.m_position) * ease;
            kept.m_color = kept.m_color + (light.m_color - kept.m_color) * ease;
            kept.m_radius += (light.m_radius - kept.m_radius) * ease;
            kept.m_start += (light.m_start - kept.m_start) * ease;
            kept.m_flicker = light.m_flicker;
            kept.m_specular = light.m_specular;
            match->m_seen = true;
        } else {
            s_tracked.push_back(TrackedLight { light, 0.0f, true });
        }
    }

    s_lights.clear();

    for (size_t i = 0; i < s_tracked.size();) {
        TrackedLight& tracked = s_tracked[i];

        if (tracked.m_seen) {
            tracked.m_weight += dt / 0.3f;
        } else {
            tracked.m_weight -= dt / 0.6f;
        }

        tracked.m_weight = std::min(tracked.m_weight, 1.0f);

        if (tracked.m_weight <= 0.0f && !tracked.m_seen) {
            s_tracked[i] = s_tracked.back();
            s_tracked.pop_back();
            continue;
        }

        float w = std::max(tracked.m_weight, 0.0f);
        GxLightingLight light = tracked.m_light;
        light.m_color = light.m_color * (w * w * (3.0f - 2.0f * w));
        s_lights.push_back(light);
        i++;
    }
}

// a colour scaled to luminance 1 (a tint), white when it is too dark to say
void Tint(const C3Vector& c, float* out) {
    float lum = Luminance(c);

    if (lum < 0.02f) {
        out[0] = out[1] = out[2] = 1.0f;
        return;
    }

    out[0] = fminf(c.x / lum, 3.0f);
    out[1] = fminf(c.y / lum, 3.0f);
    out[2] = fminf(c.z / lum, 3.0f);
}

// g_gxLightingGpu from the params and the lights (render-relative), in the view the world draws with
void WriteGpu(const GxLightingParams& p) {
    GxLightingGpu& g = g_gxLightingGpu;
    std::memset(&g, 0, sizeof(g));
    uint32_t features = p.m_features & (GxLighting_PerPixel | GxLighting_Dynamic | GxLighting_Specular | GxLighting_Hemisphere | GxLighting_Pbr);

    // ponytail: SelectLights picks kGxLightingMaxLights, looped per pixel; only lights still fading out can pass the cap
    // here, and the dimmest go. Badgermilk bins up to 256 into view clusters with a compute pass (plan step 4)
    if (s_lights.size() > kGxLightingMaxLights) {
        std::partial_sort(s_lights.begin(), s_lights.begin() + kGxLightingMaxLights, s_lights.end(), [](const GxLightingLight& a, const GxLightingLight& b) {
            return Luminance(a.m_color) > Luminance(b.m_color);
        });
        s_lights.resize(kGxLightingMaxLights);
    }

    if (s_lights.empty()) {
        features &= ~GxLighting_Dynamic;
    }

    if (features) {
        CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
        const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
        C44Matrix viewInv = view.Inverse();
        std::memcpy(g.m_view, &view, sizeof(g.m_view));
        std::memcpy(g.m_viewInv, &viewInv, sizeof(g.m_viewInv));
        g.m_flags[0] = features;
        g.m_flags[1] = (features & GxLighting_Dynamic) ? static_cast<uint32_t>(s_lights.size()) : 0u;
        std::memcpy(&g.m_flags[2], &p.m_roughnessBias, sizeof(float));   // PBR-lite: the shaders read it as float bits
        // ARMORMETAL: plate and mail (metalness from the CVars; plate smoother than the chain links of mail)
        g.m_armor[0] = p.m_metalPlate;
        g.m_armor[1] = 0.4f;
        g.m_armor[2] = p.m_metalMail;
        g.m_armor[3] = 0.5f;
        g.m_flags[3] = p.m_debug;
        Tint(p.m_skyColor, g.m_skyTint);
        Tint(p.m_groundColor, g.m_groundTint);
        g.m_groundTint[3] = p.m_wmoNormal;   // WMONORMALS
        g.m_skyTint[3] = (features & GxLighting_Hemisphere) ? p.m_hemisphere : 0.0f;

        // world up (z) in view space; the time for the flicker
        C3Vector up(view.c0, view.c1, view.c2);
        float len = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
        len = len > 0.0f ? 1.0f / len : 0.0f;
        g.m_upView[0] = up.x * len;
        g.m_upView[1] = up.y * len;
        g.m_upView[2] = up.z * len;
        g.m_upView[3] = p.m_time;
        g.m_lightParams[0] = p.m_lightWrap;
        g.m_lightParams[1] = p.m_dynamicIntensity;
        g.m_lightParams[2] = p.m_specularM2;
        g.m_lightParams[3] = p.m_specularWmo;

        for (uint32_t i = 0; i < g.m_flags[1]; i++) {
            const GxLightingLight& light = s_lights[i];
            GxLightingGpuLight& gpu = g.m_lights[i];
            C3Vector v = Transform(light.m_position, view);
            gpu.m_pos[0] = v.x;
            gpu.m_pos[1] = v.y;
            gpu.m_pos[2] = v.z;
            gpu.m_radius = light.m_radius;
            gpu.m_color[0] = light.m_color.x;
            gpu.m_color[1] = light.m_color.y;
            gpu.m_color[2] = light.m_color.z;
            gpu.m_start = light.m_start;
            gpu.m_flicker = light.m_flicker;
            // a seed from the position, so a light flickers the same way every frame
            gpu.m_seed = fmodf(fabsf(light.m_position.x * 12.9898f + light.m_position.y * 78.233f + light.m_position.z * 37.719f), 6.2831853f);
            gpu.m_specular = light.m_specular;
        }
    }

    g_gxLightingGpuSerial++;
}

}

void WorldLightingRegisterCVars() {
    s_cv.exposure = Reg("hdrExposure", "HDR exposure (D3D11/Vulkan)", "1");
    s_cv.tonemap = Reg("hdrTonemap", "HDR tonemap: 0 clamp (stock), 1 neutral, 2 ACES", "1");
    s_cv.bloom = Reg("bloom", "Bloom intensity, 0 = off (D3D11/Vulkan)", "0");
    s_cv.bloomThreshold = Reg("bloomThreshold", "Bloom threshold (linear light; the stock shaders never pass 1)", "0.6");
    s_cv.ssao = Reg("lightingSsao", "Post-pass ambient occlusion intensity, 0 = off (D3D11/Vulkan)", "0");
    s_cv.ssaoRadius = Reg("lightingSsaoRadius", "Post-pass SSAO radius in yards", "1.5");
    s_cv.ssaoDistance = Reg("lightingSsaoDistance", "Post-pass SSAO fades out by this distance in yards", "200");
    s_cv.fogScattering = Reg("fogScattering", "Fog tinted toward the sun's colour around the sun, 0 = off", "0");
    s_cv.debug = Reg("lightingDebug", "1 shows the SSAO term alone; 2 lit surfaces by kind, 3 dynamic light alone; 4, 5 the SSAO inputs", "0");
    // the shader half (lit shader pack in custom.MPQ; D3D11 and Vulkan)
    s_cv.master = Reg("shaderLighting", "Master switch for the light* shader lighting, 0 = all off (D3D11/Vulkan)", "0");
    s_cv.perPixel = Reg("lightPerPixel", "M2, WMO and terrain lit per pixel with their stock formulas (D3D11/Vulkan)", "0");
    s_cv.dynamic = Reg("lightDynamic", "Point lights of models and WMOs light the world per pixel (D3D11/Vulkan)", "0");
    s_cv.dynamicIntensity = Reg("lightDynamicIntensity", "Dynamic light intensity (0-8)", "1");
    s_cv.dynamicM2 = Reg("lightDynamicM2", "Model (torch, brazier) light strength (0-8)", "0.6");
    s_cv.dynamicWmo = Reg("lightDynamicWmo", "WMO (building) light strength (0-8)", "0.5");
    s_cv.dynamicDistance = Reg("lightDynamicDistance", "Dynamic lights reach this far from the camera, yards (20-500)", "100");
    s_cv.wmoInsideOnly = Reg("lightDynamicWmoInsideOnly", "1 = an interior WMO room's lights only while the camera is in its box (whole rooms of lamps switch as you walk between them); 0 = every room within reach (default)", "0");
    s_cv.dynamicMinRadius = Reg("lightDynamicMinRadius", "A model light reaches at least this far, yards (the stock falloff has no end)", "6");
    s_cv.replaceStock = Reg("lightReplaceStock", "With lightDynamic on, the stock per-object point lights are off: the dynamic lights replace them (1 = units keep theirs, 2 = units too)", "1");
    s_cv.flicker = Reg("lightFlicker", "Flame lights flicker this much (0-1)", "0");
    s_cv.wrap = Reg("lightWrap", "Dynamic light wraps round the terminator (0-1)", "0.25");
    s_cv.specular = Reg("lightSpecular", "Specular on WMO specular shaders and shiny models (D3D11/Vulkan)", "0");
    s_cv.specularM2 = Reg("lightSpecularM2", "Model specular strength (0-4)", "0.3");
    s_cv.specularWmo = Reg("lightSpecularWmo", "WMO specular strength (0-4)", "1");
    s_cv.skyAmbient = Reg("lightSkyAmbient", "Ambient tinted by the sky above and the ground below, 0 = off (0-1)", "0");
    s_cv.pbr = Reg("lightPbr", "PBR-lite: GGX specular from the sun and dynamic lights, metal on armour and metal parts (D3D11/Vulkan; opt-in until signed off)", "0");
    s_cv.roughnessBias = Reg("lightRoughnessBias", "PBR-lite: added to every surface's roughness, negative = shinier (-1 to 1)", "0");
    s_cv.metalPlate = Reg("lightMetalPlate", "PBR-lite: metalness of equipped plate armour (0-1)", "0.65");
    s_cv.metalMail = Reg("lightMetalMail", "PBR-lite: metalness of equipped mail armour (0-1)", "0.4");
    s_cv.wmoNormalMaps = Reg("wmoNormalMaps", "WMO stone and wood relief from the _n normal maps, shader lighting only (D3D11/Vulkan)", "1");
    s_cv.wmoNormalStrength = Reg("wmoNormalStrength", "WMO normal map strength (0-2)", "1");
    // last: Features() takes a registered hdr to mean they all are
    s_cv.hdr = Reg("hdr", "HDR world target with a tonemap (D3D11; Vulkan tonemaps the 8-bit scene)", "0");
    ConsoleCommandRegister("lightingDump", CCommand_LightingDump, CATEGORY_GRAPHICS, "Prints the dynamic lights gathered next frame and the stock model lights within 30 yd");
}

void WorldLightingBeginWorld() {
    s_inWorld = false;
    uint32_t features = g_gxLighting ? Features() : 0;

    // the stock per-object point lights give way to ours (M2Light.cpp CM2Lighting::AddLight); D3D9 (no g_gxLighting)
    // and lightDynamic off keep them
    g_m2ShaderOwnedLightScene = (features & GxLighting_Dynamic) && s_cv.replaceStock->m_intValue > 0 ? World::s_m2Scene : nullptr;
    g_m2KeepStockPointLights = s_cv.replaceStock->m_intValue == 1 ? KeepStockPointLights : nullptr;   // UNITLIGHTS (2 = units too)

    // all off: the device is never called, the frame is the stock one
    if (!features) {
        return;
    }

    DayNight::DNInfo* info = DayNight::GetInfo();
    BuildParams(s_params, features, info ? info->m_cameraPos : C3Vector(0.0f, 0.0f, 0.0f));
    g_gxLighting->BeginWorld(s_params);
    s_inWorld = true;
}

void WorldLightingSetupView(const C3Vector& camera) {
    if (!s_inWorld || !g_gxLighting) {
        return;
    }

    // the view matrix is render-relative (CWorldScene::s_activeWorldView): so is the camera
    const C3Vector& origin = CWorldScene::s_activeWorldView;
    BuildParams(s_params, s_params.m_features, camera);
    s_params.m_cameraPosition = camera - origin;
    g_gxLighting->SetupView(s_params);

    // the shader half: the lights in reach (world space), smoothed across frames, moved into the render origin
    s_lights.clear();

    if (s_params.m_features & GxLighting_Dynamic) {
        float reach = std::clamp(s_cv.dynamicDistance->m_floatValue, 20.0f, 500.0f);
        s_gathered.clear();
        s_wmoDeduped = 0;
        GatherM2Lights(World::s_m2Scene, camera, reach);
        GatherWmoLights(camera, reach);
        SelectLights();

        if (s_dumpPending) {
            s_dumpPending = false;
            DumpLights(World::s_m2Scene, camera);
        }

        SmoothLights();

        for (GxLightingLight& light : s_lights) {
            light.m_position = light.m_position - origin;
        }
    } else {
        s_tracked.clear();
    }

    WriteGpu(s_params);
}

void WorldLightingOpaqueDone() {
    if (s_inWorld && g_gxLighting) {
        g_gxLighting->OpaqueDone();
    }
}

void WorldLightingEndWorld() {
    if (s_inWorld && g_gxLighting) {
        g_gxLighting->EndWorld();
    }

    // the lit shaders take their stock path outside the world pass
    if (g_gxLightingGpu.m_flags[0]) {
        g_gxLightingGpu.m_flags[0] = 0;
        g_gxLightingGpuSerial++;
    }

    s_inWorld = false;
}
