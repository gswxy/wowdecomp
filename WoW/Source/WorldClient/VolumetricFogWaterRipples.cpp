// VFOG water ripples and wakes: native port of coa-vfog src/water_ripples.cpp, water_contacts.cpp, engine_actors.cpp
// and client_ripple_sprites.cpp (@ 4e31ddf, GPLv3). docs/VFOG_PORT.md "Water", ripples.
//
// The units in water around the camera target (players, creatures, pets, mounts: the visible object list, coa
// engine_actors) feed a tracker (coa water_contacts: held footprints along each unit's path, a splash on entering) that
// disturbs a 30 Hz height field: VFogRippleStep ping-pongs two Gr1616F maps (height, previous height) of 0.125 yd
// texels around the camera target, 1536^2 (768^2 at vfogWaterQuality 1; coa: 512^2 / 256^2). VFogWater samples it at s12 with c65-c67.
// Moving ships (transport WMOs in water) press their waterline section into a second, wide level instead: wxl-water's
// level 1 as it ships (1024^2 over 512 yd, an 83 ms step), sampled on its own (VFogWater s15 c70-c71, CataWater s3 c32-c33).
// Nothing runs without water in view (called from the water pass) or when nothing moved for 15 s (coa quiet steps).
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/Movement_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/ChatFrame.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/World.h"

namespace VolumetricFog {
namespace {

// ---- coa engine_actors.h / water_contacts.h / water_ripples.h / water_renderer.cpp constants -----------------------
constexpr uint32_t kMaxWaterContacts = 32;
constexpr float kWaterContactRange = 48.0f;
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;

constexpr size_t kMaxDepartedWaterEntries = 256;
constexpr float kWaterSplashRearmDepthPerHeight = 0.15f;
constexpr double kMinWaterSplashIntervalSeconds = 2.0;
constexpr double kStaleWaterTrackSeconds = 0.5;
constexpr double kWaterFootprintFadeSeconds = 1.0;
constexpr double kWaterWakeRampSeconds = 0.3;
constexpr float kMaxWaterWakeSpeed = 40.0f;
constexpr float kWaterWakeStepSlack = 0.5f;
constexpr float kClientSplashDepthPerHeight = 0.4f;
constexpr float kClientFullStrength = 1.0f;
constexpr float kClientDeepestStrength = 0.5f;
constexpr float kFootprintRadiusPerCollisionRadius = 1.25f;
constexpr float kMinFootprintRadius = 0.3f;
constexpr float kMaxFootprintRadius = 6.0f;
constexpr float kFullImmersionDepthPerHeight = 0.25f;
constexpr float kFullyImmersedFootprintLevel = 3.0f;
constexpr float kStandingFootprintShare = 0.2f;
constexpr float kFullWakeSpeed = 4.5f;
constexpr float kSplashRadiusPerCollisionRadius = 3.0f;
constexpr float kMinSplashRadius = 0.75f;
constexpr float kMaxSplashRadius = 9.0f;
constexpr float kEntryImpulsePerStrength = -1.5f;
constexpr double kDepartedEntrySeconds = 30.0;

constexpr float kWaterRippleTexelYards = 0.125f;
constexpr double kWaterRippleStepsPerSecond = 30.0;
constexpr int kMaxWaterRippleStepsPerFrame = 4;
constexpr uint32_t kMaxWaterRippleDisturbances = 48;
constexpr int kWaterRippleTexelsLow = 768;     // coa 256 / 512; tripled (192 yd) so rain rings reach three times as far, same 0.125 yd texel
constexpr int kWaterRippleTexels = 1536;
constexpr uint32_t kWaterRippleQuietSteps = 450;
constexpr float kMaxWindowCentreYards = 100000.0f;
constexpr float kHeldFootprint = 1.0f;
constexpr float kAddedImpulse = 0.0f;
constexpr uint32_t kStepRegister = 0;
constexpr uint32_t kFirstSegmentRegister = 1;
constexpr uint32_t kFirstShapeRegister = kFirstSegmentRegister + kMaxWaterRippleDisturbances;
constexpr uint32_t kKeepRegister = 97;      // new: 0 = clear (VFogRippleStep.hlsl)

constexpr double kRippleGapSeconds = 1.0;
constexpr float kRippleEdgeRampPerExtent = 1.0f / 16.0f;
constexpr float kRippleFadeYards = 4.0f;
constexpr double kClientSpriteGraceSeconds = 1.0;

// ---- contacts (coa engine_actors.cpp) ----------------------------------------------------------------------------
struct WaterContact {
    uint64_t guid = 0;
    float position[3] = {};
    float surface = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    bool swimming = false;
};

struct WaterContactFrame {
    WaterContact contacts[kMaxWaterContacts];
    uint32_t count = 0;
};

float WaterContactDepth(const WaterContact& contact) {
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height) {
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

bool FiniteWithin(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c) {
    for (float axis : c.position) {
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return false;
        }
    }
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f && FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius)
        && c.height > 0.0f && FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3]) {
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3]) {
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    return aDistance != bDistance ? aDistance < bDistance : a.guid < b.guid;
}

// coa SelectWaterContact: the 32 nearest plausible contacts within 48 yd, nearest first.
void SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3]) {
    if (!PlausibleContact(contact) || SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange) {
        return;
    }
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre)) {
        ++index;
    }
    if (index == kMaxWaterContacts) {
        return;
    }
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i) {
        frame.contacts[i] = frame.contacts[i - 1];
    }
    frame.contacts[index] = contact;
    frame.count = count;
}

// coa WalkVisibleUnits / ReadWaterContact, through the decomp's types: a unit whose map entity found liquid whose surface
// crosses its body (CMapStaticEntity m_staticFlags 0x20 | 0x100, the flags the client's own ripple emitter tests).
void CaptureWaterContacts(const float centre[3], WaterContactFrame& out) {
    out.count = 0;
    ObjectMgr* manager = ClntObjMgrGetCurrent();
    if (!manager) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return;
        }
    }
    for (CGObject_C* object = manager->m_visibleObjects.Head(); object; object = manager->m_visibleObjects.Next(object)) {
        if (!object->m_obj || !(object->m_obj->m_type & TYPE_UNIT)) {
            continue;
        }
        const CGUnit_C* unit = static_cast<const CGUnit_C*>(object);
        const CMapEntity* entity = unit->m_worldObject;
        if (!entity || !(entity->m_staticFlags & kEntityLiquidFound) || !(entity->m_staticFlags & kEntitySurfaceCrossesBody)) {
            continue;
        }
        const CMovement_C& movement = unit->m_movementData;
        WaterContact contact;
        contact.guid = object->m_obj->m_guid.m_guid;
        const C3Vector& position = movement.m_transportGuid ? entity->m_position : movement.m_position;
        contact.position[0] = position.x;
        contact.position[1] = position.y;
        contact.position[2] = position.z;
        contact.surface = entity->m_liquidHeight;
        contact.radius = movement.m_collisionRadius;
        contact.height = movement.m_collisionHeight;
        contact.swimming = (movement.m_flags & kSwimmingMovementFlag) != 0;
        SelectWaterContact(out, contact, centre);
    }
}

// ---- tracker (coa water_contacts.cpp) ------------------------------------------------------------------------------
struct WaterRippleDisturbance {
    float from[2] = {};
    float to[2] = {};
    float radius = 0.0f;
    float amplitude = 0.0f;
    bool held = false;
};

struct WaterEntryState {
    bool armed = false;
    double splashedAt = -std::numeric_limits<double>::infinity();
};

struct WaterFootprint {
    float from[2] = {};
    double fromSeconds = 0.0;
    float stamped[2] = {};
    double placedAt = 0.0;
    double steppedAt = -std::numeric_limits<double>::infinity();
    float radius = 0.0f;
    float level = 0.0f;
    float motion = 0.0f;
};

struct WaterContactTrack {
    uint64_t guid = 0;
    float position[3] = {};
    double seenAt = 0.0;
    WaterEntryState entry;
    WaterFootprint footprint;
    float splashRadius = 0.0f;
    float pendingSplash = 0.0f;
};

struct DepartedWaterEntry {
    uint64_t guid = 0;
    WaterEntryState entry;
    double seenAt = 0.0;
};

float ClientRippleStrength(float depth, float height) {
    const float limit = ClientRippleDepthLimit(height);
    if (depth <= 0.5f * limit) {
        return kClientFullStrength;
    }
    return std::max(kClientDeepestStrength, kClientDeepestStrength + (limit - depth) / limit);
}

float WaterFootprintRadius(float collisionRadius) {
    return std::clamp(kFootprintRadiusPerCollisionRadius * collisionRadius, kMinFootprintRadius, kMaxFootprintRadius);
}

float WaterFootprintLevel(float depth, float height) {
    const float immersion = std::clamp(depth / (kFullImmersionDepthPerHeight * height), 0.0f, 1.0f);
    return kFullyImmersedFootprintLevel * immersion * ClientRippleStrength(depth, height);
}

float WaterWakeMotion(float speed) {
    return std::clamp(speed / kFullWakeSpeed, 0.0f, 1.0f);
}

float WaterFootprintDepthShare(float motion) {
    return kStandingFootprintShare + (1.0f - kStandingFootprintShare) * motion;
}

double SmoothUnitStep(double x) {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float WaterFootprintFade(double sincePlaced, double sinceSeen) {
    return static_cast<float>(SmoothUnitStep(sincePlaced / kWaterFootprintFadeSeconds) * SmoothUnitStep(1.0 - sinceSeen / kStaleWaterTrackSeconds));
}

bool PlausibleWaterWakeStep(float distance, double seconds) {
    return distance <= kMaxWaterWakeSpeed * std::max(seconds, 0.0) + kWaterWakeStepSlack;
}

float WaterSplashRadius(float collisionRadius) {
    return std::clamp(kSplashRadiusPerCollisionRadius * collisionRadius, kMinSplashRadius, kMaxSplashRadius);
}

float WaterEntryImpulse(float depth, float height) {
    return kEntryImpulsePerStrength * ClientRippleStrength(depth, height);
}

bool TakeEntrySplash(WaterEntryState& entry, const WaterContact& contact, double seconds) {
    if (contact.swimming) {
        entry.armed = false;
        return false;
    }
    const float depth = WaterContactDepth(contact);
    if (depth <= kWaterSplashRearmDepthPerHeight * contact.height) {
        entry.armed = true;
    }
    if (!entry.armed || depth <= kClientSplashDepthPerHeight * contact.height) {
        return false;
    }
    entry.armed = false;
    if (seconds - entry.splashedAt < kMinWaterSplashIntervalSeconds) {
        return false;
    }
    entry.splashedAt = seconds;
    return true;
}

float GroundDistance(const float a[3], const float b[3]) {
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

void PlaceFootprint(WaterContactTrack& track, const float position[3], double seconds) {
    WaterFootprint& footprint = track.footprint;
    for (int axis = 0; axis < 2; ++axis) {
        footprint.from[axis] = footprint.stamped[axis] = position[axis];
    }
    footprint.fromSeconds = seconds;
    footprint.placedAt = seconds;
    footprint.motion = 0.0f;
    footprint.steppedAt = -std::numeric_limits<double>::infinity();
}

void FollowMotion(WaterFootprint& footprint, float travelled, double stepSeconds) {
    const double elapsed = stepSeconds - footprint.steppedAt;
    footprint.steppedAt = stepSeconds;
    if (!(elapsed > 0.0) || !std::isfinite(elapsed)) {
        return;
    }
    const float ramp = static_cast<float>(elapsed / kWaterWakeRampSeconds);
    const float target = WaterWakeMotion(static_cast<float>(travelled / elapsed));
    footprint.motion += std::clamp(target - footprint.motion, -ramp, ramp);
}

float PathFraction(const WaterContactTrack& track, double stepSeconds) {
    const double span = track.seenAt - track.footprint.fromSeconds;
    if (!(span > 0.0)) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp((stepSeconds - track.footprint.fromSeconds) / span, 0.0, 1.0));
}

WaterRippleDisturbance FootprintStep(WaterContactTrack& track, double stepSeconds) {
    WaterFootprint& footprint = track.footprint;
    const float along = PathFraction(track, stepSeconds);
    WaterRippleDisturbance step;
    for (int axis = 0; axis < 2; ++axis) {
        step.from[axis] = footprint.stamped[axis];
        step.to[axis] = footprint.from[axis] + (track.position[axis] - footprint.from[axis]) * along;
        footprint.stamped[axis] = step.to[axis];
    }
    FollowMotion(footprint, std::hypot(step.to[0] - step.from[0], step.to[1] - step.from[1]), stepSeconds);
    step.radius = footprint.radius;
    step.amplitude = footprint.level * WaterFootprintFade(stepSeconds - footprint.placedAt, stepSeconds - track.seenAt) * WaterFootprintDepthShare(footprint.motion);
    step.held = true;
    return step;
}

class WaterContactTracker {
public:
    void Reset() {
        m_tracks.clear();
        m_departed.clear();
        m_seconds = -1.0;
    }

    void Update(const WaterContactFrame& frame, double seconds) {
        if (m_seconds >= 0.0 && seconds < m_seconds) {
            Reset();
        }
        for (uint32_t i = 0; i < frame.count; ++i) {
            const WaterContact& contact = frame.contacts[i];
            bool created = false;
            WaterContactTrack& track = TrackOf(contact.guid, created);
            if (!created && track.seenAt == seconds) {
                continue;
            }
            Follow(track, contact, seconds, created);
        }
        m_seconds = seconds;
        DropStaleTracks(seconds);
    }

    void PlaceFootprints(double seconds) {
        for (WaterContactTrack& track : m_tracks) {
            PlaceFootprint(track, track.position, seconds);
        }
    }

    uint32_t TakeDisturbances(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = TakeFootprints(stepSeconds, true, out, capacity);
        count += TakeFootprints(stepSeconds, false, out + count, capacity - count);
        return count + TakeSplashes(out + count, capacity - count);
    }

    bool Emitting() const { return !m_tracks.empty(); }

private:
    WaterContactTrack& TrackOf(uint64_t guid, bool& created) {
        for (WaterContactTrack& track : m_tracks) {
            if (track.guid == guid) {
                created = false;
                return track;
            }
        }
        created = true;
        m_tracks.emplace_back();
        m_tracks.back().guid = guid;
        return m_tracks.back();
    }

    WaterEntryState FirstEntryState(uint64_t guid) {
        auto departed = std::find_if(m_departed.begin(), m_departed.end(), [guid](const DepartedWaterEntry& e) { return e.guid == guid; });
        if (departed == m_departed.end()) {
            return {};
        }
        const WaterEntryState entry = departed->entry;
        m_departed.erase(departed);
        return entry;
    }

    void Follow(WaterContactTrack& track, const WaterContact& contact, double seconds, bool created) {
        if (created) {
            track.entry = FirstEntryState(contact.guid);
        }
        if (created || !PlausibleWaterWakeStep(GroundDistance(track.position, contact.position), seconds - track.seenAt)) {
            PlaceFootprint(track, contact.position, seconds);
        } else {
            for (int axis = 0; axis < 2; ++axis) {
                track.footprint.from[axis] = track.position[axis];      // coa MoveFootprint
            }
            track.footprint.fromSeconds = track.seenAt;
        }
        std::copy(contact.position, contact.position + 3, track.position);
        track.seenAt = seconds;
        const float depth = WaterContactDepth(contact);
        track.footprint.radius = WaterFootprintRadius(contact.radius);
        track.footprint.level = WaterFootprintLevel(depth, contact.height);
        track.splashRadius = WaterSplashRadius(contact.radius);
        if (TakeEntrySplash(track.entry, contact, seconds)) {
            track.pendingSplash += WaterEntryImpulse(depth, contact.height);
        }
    }

    void RememberEntry(const WaterContactTrack& track) {
        const DepartedWaterEntry memory = { track.guid, track.entry, track.seenAt };
        auto same = std::find_if(m_departed.begin(), m_departed.end(), [&track](const DepartedWaterEntry& d) { return d.guid == track.guid; });
        if (same != m_departed.end()) {
            *same = memory;
        } else if (m_departed.size() < kMaxDepartedWaterEntries) {
            m_departed.push_back(memory);
        } else {
            *std::min_element(m_departed.begin(), m_departed.end(), [](const DepartedWaterEntry& a, const DepartedWaterEntry& b) { return a.seenAt < b.seenAt; }) = memory;
        }
    }

    void DropStaleTracks(double seconds) {
        const auto stale = [seconds](const WaterContactTrack& track) { return seconds - track.seenAt > kStaleWaterTrackSeconds; };
        for (const WaterContactTrack& track : m_tracks) {
            if (stale(track)) {
                RememberEntry(track);
            }
        }
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(), stale), m_tracks.end());
        m_departed.erase(std::remove_if(m_departed.begin(), m_departed.end(), [seconds](const DepartedWaterEntry& d) { return seconds - d.seenAt > kDepartedEntrySeconds; }), m_departed.end());
    }

    uint32_t TakeFootprints(double stepSeconds, bool fresh, WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = 0;
        for (WaterContactTrack& track : m_tracks) {
            if ((track.seenAt == m_seconds) == fresh && count < capacity) {
                out[count++] = FootprintStep(track, stepSeconds);
            }
        }
        return count;
    }

    uint32_t TakeSplashes(WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = 0;
        for (WaterContactTrack& track : m_tracks) {
            if (track.pendingSplash == 0.0f || count == capacity) {
                continue;
            }
            WaterRippleDisturbance& splash = out[count++];
            splash = {};
            for (int axis = 0; axis < 2; ++axis) {
                splash.from[axis] = splash.to[axis] = track.position[axis];
            }
            splash.radius = track.splashRadius;
            splash.amplitude = track.pendingSplash;
            track.pendingSplash = 0.0f;
        }
        return count;
    }

    std::vector<WaterContactTrack> m_tracks;
    std::vector<DepartedWaterEntry> m_departed;
    double m_seconds = -1.0;
};

// ---- moving ship hulls (wxl-water GatherHulls / BuildHullProfile / HullShape, Stage 7c) -----------------------------
// Moving TRANSPORT / MO_TRANSPORT WMOs whose model bottom sits under a liquid surface (airborne zeppelins fail the test)
// press their own waterline section into the field. The section is the WMO's triangles cut at the local waterline,
// reduced to 12 half-beams along local x, cut once per root, and drawn as wxl-water's 24-point polygon through the slice
// midpoints, fanned from its centroid, additively into the wide level only, pulsed at 1 Hz (wxl-water SplatHulls).
// Strength 0.8 * min(speed / 10 yd/s, 1) (Cata's), none when docked (under 0.5 yd/s), times waterShipWakes.
constexpr int kHullSlices = 12;
constexpr int kHullPoints = kHullSlices * 2;
constexpr uint32_t kMaxHulls = 8;                            // wxl-water kRipMaxHulls
constexpr double kHullSpeedSeconds = 0.1;
constexpr double kStaleHullSeconds = 2.0;
constexpr float kHullFullSpeed = 10.0f;
constexpr float kHullDockedShare = 0.05f;
constexpr float kHullStrength = 0.8f;
// The wide level (wxl-water ripLvl_[1] = { 512 yd, 50 ms, splat 0.25 }): 1024^2 of 0.5 yd texels, the step stretched by
// its shipped ripplespeed 0.6 (Settings.ini; RippleTick: unsigned(50 / 0.6 + 0.5) = 83 ms), at most 2 steps a frame, the
// close level's propagate (damping 0.97, the outer 1/16 fading out) with no disturbances of its own.
constexpr int kWideTexels = 1024;
constexpr float kWideTexelYards = 512.0f / kWideTexels;
constexpr double kWideStepsPerSecond = 1000.0 / 83.0;
constexpr int kMaxWideStepsPerFrame = 2;
constexpr float kWideSplat = 0.25f;
constexpr float kPerturbCentre = 248.0f / 255.0f;            // perturbtex.blp at uv (0.5, 0.5), where every hull vertex samples it
constexpr double kWakePulseSeconds = 1.0;

struct HullProfile {
    const CMapObj* root = nullptr;
    float zw = 0.0f;
    float x0 = 0.0f;
    float x1 = 0.0f;
    float hb[kHullSlices] = {};
    bool ok = false;
};

// wxl-water BuildHullProfile: cut the loaded groups at local z = zw (two passes: x extent, then the widest |y| per slice).
bool BuildHullProfile(const CMapObj* root, float zw, HullProfile& out) {
    if (!root || !root->m_isGroupLoaded || !root->m_groupInfo || root->m_groupInfoCount <= 0 || root->m_groupInfoCount > 512) {
        return false;
    }
    float x0 = 1e30f;
    float x1 = -1e30f;
    float beamCap = 0.0f;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            if (!(x1 - x0 > 2.0f)) {
                return false;
            }
            out.x0 = x0;
            out.x1 = x1;
            std::fill(std::begin(out.hb), std::end(out.hb), 0.0f);
        }
        const float w = pass == 1 ? (x1 - x0) / static_cast<float>(kHullSlices) : 1.0f;
        for (int i = 0; i < root->m_groupInfoCount; ++i) {
            const CAaBox& box = root->m_groupInfo[i].m_boundingBox;
            if (zw < box.b.z || zw > box.t.z) {
                continue;
            }
            const CMapObjGroup* g = root->m_mapObjGroupArray[i];
            if (!g || !(g->m_unkLoadedFlag & 1) || (g->m_flags & 0x80)) {
                continue;
            }
            const uint32_t nTri = static_cast<uint32_t>(g->m_polyListSize);
            if (!nTri || nTri > (1u << 20) || !g->m_indices || !g->m_vertexList) {
                continue;
            }
            if (pass == 0) {
                beamCap = std::max(beamCap, 0.5f * (box.t.y - box.b.y));
            }
            for (uint32_t t = 0; t < nTri; ++t) {
                float sx[2];
                float sy[2];
                int n = 0;
                for (int e = 0; e < 3 && n < 2; ++e) {
                    const C3Vector& a = g->m_vertexList[g->m_indices[t * 3 + e]];
                    const C3Vector& b = g->m_vertexList[g->m_indices[t * 3 + (e + 1) % 3]];
                    const float da = a.z - zw;
                    const float db = b.z - zw;
                    if ((da < 0.0f) == (db < 0.0f)) {
                        continue;
                    }
                    const float f = da / (da - db);
                    sx[n] = a.x + f * (b.x - a.x);
                    sy[n] = a.y + f * (b.y - a.y);
                    ++n;
                }
                if (n != 2) {
                    continue;
                }
                if (pass == 0) {
                    x0 = std::min({ x0, sx[0], sx[1] });
                    x1 = std::max({ x1, sx[0], sx[1] });
                    continue;
                }
                const int ia = sx[0] <= sx[1] ? 0 : 1;
                const int ib = 1 - ia;
                const float ax = sx[ia], ay = sy[ia], bx = sx[ib], by = sy[ib];
                const int i0 = std::clamp(static_cast<int>((ax - x0) / w), 0, kHullSlices - 1);
                const int i1 = std::clamp(static_cast<int>((bx - x0) / w), 0, kHullSlices - 1);
                for (int s = i0; s <= i1; ++s) {
                    for (float cut : { std::max(ax, x0 + w * s), std::min(bx, x0 + w * (s + 1)) }) {
                        const float f = bx > ax ? (cut - ax) / (bx - ax) : 0.0f;
                        out.hb[s] = std::max(out.hb[s], std::fabs(ay + f * (by - ay)));
                    }
                }
            }
        }
    }
    for (float& h : out.hb) {
        h = std::min(h, beamCap);
    }
    out.root = root;
    out.zw = zw;
    out.ok = std::any_of(std::begin(out.hb), std::end(out.hb), [](float h) { return h > 0.5f; });
    return out.ok;
}

std::vector<HullProfile> s_hullCache;

// wxl-water HullProfileFor: one section per root, recut only when the local waterline drifts over 1 yd.
const HullProfile* HullProfileFor(const CMapObj* root, float zw) {
    for (HullProfile& p : s_hullCache) {
        if (p.root == root) {
            return (p.ok && std::fabs(p.zw - zw) <= 1.0f) ? &p : (BuildHullProfile(root, zw, p) ? &p : nullptr);
        }
    }
    if (s_hullCache.size() >= 32) {
        s_hullCache.erase(s_hullCache.begin());     // ponytail: LRU-less, 32 roots is plenty (as wxl-water)
    }
    HullProfile p;
    if (!BuildHullProfile(root, zw, p)) {
        p.root = root;
        p.ok = false;
    }
    s_hullCache.push_back(p);
    return s_hullCache.back().ok ? &s_hullCache.back() : nullptr;
}

struct HullTrack {
    uint64_t guid = 0;
    float x = 0.0f;
    float y = 0.0f;
    double at = 0.0;
    double seen = 0.0;
    float speedShare = 0.0f;
    float speed = 0.0f;         // yd/s (waterWakeDebug)
};

std::vector<HullTrack> s_hullTracks;

// A moving hull pressed into the height field: its waterline footprint in world space, bow-first down one side and back
// up the other (wxl-water RippleHullFill).
struct HullFill {
    float strength = 0.0f;
    int n = 0;
    double x[kHullPoints] = {};
    double y[kHullPoints] = {};
};

// wxl-water HullShape. m: the WMO def's local -> world (translation m[12..14]); box: the root's local bounds.
void HullShape(const float* m, const CAaBox& box, const HullProfile* p, float strength, HullFill& h) {
    h = {};
    h.strength = strength;
    auto emit = [&](float lx, float ly) {
        h.x[h.n] = static_cast<double>(lx * m[0] + ly * m[4] + m[12]);
        h.y[h.n] = static_cast<double>(lx * m[1] + ly * m[5] + m[13]);
        ++h.n;
    };
    if (p && p->ok) {       // the hull's own section at the waterline, so bowsprits and sterncastles make no wake
        const float w = (p->x1 - p->x0) / static_cast<float>(kHullSlices);
        for (int i = 0; i < kHullSlices; ++i) {
            emit(p->x0 + w * (i + 0.5f), p->hb[i]);
        }
        for (int i = kHullSlices - 1; i >= 0; --i) {
            emit(p->x0 + w * (i + 0.5f), -p->hb[i]);
        }
        return;
    }
    // While a ship's groups are still loading: a hexagon from the model box, which includes the overhangs, so it is
    // deliberately short (wxl-water's ponytail: 0.6 of the box length, beam min(0.75 width, a / 3)).
    const bool alongX = box.t.x - box.b.x >= box.t.y - box.b.y;
    const float halfL = 0.5f * (alongX ? box.t.x - box.b.x : box.t.y - box.b.y);
    const float halfW = 0.5f * (alongX ? box.t.y - box.b.y : box.t.x - box.b.x);
    const float a = 0.6f * halfL;
    const float b = std::min(0.75f * halfW, a / 3.0f);
    const float cx = 0.5f * (box.b.x + box.t.x);
    const float cy = 0.5f * (box.b.y + box.t.y);
    const float hx[6] = { a, 0.4f * a, -0.4f * a, -a, -0.4f * a, 0.4f * a };
    const float hy[6] = { 0.0f, b, b, 0.0f, -b, -b };
    for (int i = 0; i < 6; ++i) {
        emit(cx + (alongX ? hx[i] : hy[i]), cy + (alongX ? hy[i] : hx[i]));
    }
}

// wxl-water GatherHulls, through the decomp's types.
// waterWakeDebug 1: once a second, one console line (and Logs\\wake.txt) per transport saying which gate it stopped at.
void WakeDebugPrint(const char* format, ...) {
    char text[320];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ConsoleWrite(text, DEFAULT_COLOR);
    // the lines also go to Logs\wake.txt beside the exe, truncated at the first line of the session
    static FILE* s_log = std::fopen("Logs\\wake.txt", "w");
    if (s_log) {
        std::fprintf(s_log, "%10.2f %s\n", FrameTime::s_curTimeSec, text);
        std::fflush(s_log);
    }
}

struct HullDebug {
    bool on = false;
    void Ship(uint64_t guid, int type, const char* gate, float speed, int hit, float level, float bottom, int slices, uint32_t stamps) {
        if (on) {
            WakeDebugPrint("wake: ship %08x type %d: %s | speed %.1f yd/s, liquid %d level %.1f keel %.1f, slices %d, stamps %u",
                static_cast<uint32_t>(guid), type, gate, speed, hit, level, bottom, slices, stamps);
        }
    }
};

uint32_t CaptureHulls(const float centre[3], double seconds, HullFill* out, uint32_t capacity) {
    static CVar* s_strength = CVar::LookupRegistered("waterShipWakes");
    static CVar* s_debug = CVar::LookupRegistered("waterWakeDebug");
    static double s_lastDebug = -1.0;
    HullDebug dbg;
    dbg.on = s_debug && s_debug->m_intValue != 0 && (seconds - s_lastDebug >= 1.0 || seconds < s_lastDebug);
    if (dbg.on) {
        s_lastDebug = seconds;
    }
    const float strength = s_strength ? std::clamp(s_strength->m_floatValue, 0.0f, 2.0f) : 1.0f;
    uint32_t count = 0;
    uint32_t found = 0;
    ObjectMgr* manager = ClntObjMgrGetCurrent();
    if (!manager || !(strength > 0.0f)) {
        s_hullTracks.clear();
        if (dbg.on) {
            WakeDebugPrint("wake: %s", manager ? "waterShipWakes is 0" : "no object manager");
        }
        return 0;
    }
    // Not the visible list: a game object is hidden from it until it has an M2 (CGGameObject_C ctor / PostInit
    // 0x00712F30), which WMO transports never get. The movement code's transport list holds every one.
    for (CGGameObject_C* go = MovementNextTransport(nullptr); go; go = MovementNextTransport(go)) {
        const CGObject_C* object = go;
        if (!object->m_obj || !go->m_gameObject || !go->m_typeData) {
            continue;
        }
        ++found;
        const uint64_t guid = object->m_obj->m_guid.m_guid;
        const int typeId = go->m_gameObject->m_typeID;
        if (typeId != 11 && typeId != 15) {
            dbg.Ship(guid, typeId, "not a transport type", 0, 0, 0, 0, 0, 0);
            continue;
        }
        const auto* entity = reinterpret_cast<const CMapBaseObj*>(static_cast<const CGGameObject_C_Type_WorldObj*>(go->m_typeData)->m_worldObject);
        if (!entity || !(entity->m_type & 0x8)) {
            dbg.Ship(guid, typeId, entity ? "not a WMO" : "no world object", 0, 0, 0, 0, 0, 0);
            continue;                                  // WMO only (M2 lifts skipped)
        }
        const auto* def = reinterpret_cast<const CMapObjDef*>(entity);
        const CMapObj* root = def->m_owner;
        if (!root || !root->m_isGroupLoaded) {
            dbg.Ship(guid, typeId, "WMO not loaded", 0, 0, 0, 0, 0, 0);
            continue;
        }
        const float* m = reinterpret_cast<const float*>(&def->m_mat);
        const CAaBox& box = root->m_bbox;

        auto it = std::find_if(s_hullTracks.begin(), s_hullTracks.end(), [guid](const HullTrack& t) { return t.guid == guid; });
        if (it == s_hullTracks.end()) {
            s_hullTracks.push_back({ guid, m[12], m[13], seconds, seconds });
            it = s_hullTracks.end() - 1;
        }
        it->seen = seconds;
        if (seconds - it->at >= kHullSpeedSeconds) {      // over >= 100 ms; Cata: full at 0.01 yd/ms
            it->speed = std::hypot(m[12] - it->x, m[13] - it->y) / static_cast<float>(seconds - it->at);
            it->speedShare = std::min(it->speed / kHullFullSpeed, 1.0f);
            it->x = m[12];
            it->y = m[13];
            it->at = seconds;
        }
        const float speed = it->speed;
        if (it->speedShare < kHullDockedShare) {
            dbg.Ship(guid, typeId, "docked (too slow)", speed, 0, 0, 0, 0, 0);
            continue;
        }
        const float reach = kWideTexels * kWideTexelYards * 0.5f + std::max(box.t.x - box.b.x, box.t.y - box.b.y);
        if (std::fabs(m[12] - centre[0]) > reach || std::fabs(m[13] - centre[1]) > reach) {
            dbg.Ship(guid, typeId, "out of the ripple window", speed, 0, 0, 0, 0, 0);
            continue;
        }
        if (count >= capacity) {
            dbg.Ship(guid, typeId, "over the ship cap", speed, 0, 0, 0, 0, 0);
            continue;
        }
        // waterline: box bottom under a liquid surface, box top above it. Terrain liquid only: the WMO query
        // (CMap::QueryLiquidStatusMapObjsExt) finds the ship itself first, and a ship group without MLIQ reads as
        // WMO water with no surface (CMapObjGroup::QueryLiquid level FLT_MAX), which failed the top test every time.
        const float cx = 0.5f * (box.b.x + box.t.x);
        const float cy = 0.5f * (box.b.y + box.t.y);
        C3Vector probe(cx * m[0] + cy * m[4] + box.b.z * m[8] + m[12], cx * m[1] + cy * m[5] + box.b.z * m[9] + m[13],
                       cx * m[2] + cy * m[6] + box.b.z * m[10] + m[14]);
        int32_t type = 0;
        float level = 0.0f;
        const bool hit = CMap::QueryLiquidTerrain(&probe, &type, &level, 0);
        if (!hit || level > probe.z + (box.t.z - box.b.z) * m[10]) {
            dbg.Ship(guid, typeId, hit ? "liquid over the ship top" : "keel not under terrain liquid (airborne, or no ADT water here)", speed, hit, level, probe.z, 0, 0);
            continue;
        }
        const float* w2l = reinterpret_cast<const float*>(&def->m_invMat);
        const float zw = probe.x * w2l[2] + probe.y * w2l[6] + level * w2l[10] + w2l[14];
        // wxl-water's box hexagon stands in while the groups load (p nullptr)
        const HullProfile* p = HullProfileFor(root, zw);
        HullShape(m, box, p, kHullStrength * it->speedShare * strength, out[count++]);
        dbg.Ship(guid, typeId, p ? "stamped" : "stamped (box hexagon: no waterline cut yet)", speed, hit, level, probe.z, p ? kHullSlices : 0, 1);
    }
    if (dbg.on) {
        WakeDebugPrint("wake: %u transports found, %u hulls this frame (wake window %.0f yd round the camera)", found, count, kWideTexels * kWideTexelYards);
    }
    s_hullTracks.erase(std::remove_if(s_hullTracks.begin(), s_hullTracks.end(), [seconds](const HullTrack& t) { return seconds - t.seen > kStaleHullSeconds || t.seen > seconds; }), s_hullTracks.end());
    return count;
}

// ---- rain / snow drops (wxl-water GatherRain, Cata's rain dimples) ---------------------------------------------------
// Cata: while it rains harder than 0.2 (CWeather m_rain, m_intensity), 20 dimples of 0.3 yd every 33 ms within 50 yd of
// the camera, strength -(0.2..0.4) times the close level's 1.25. That density over this window, one sim step being Cata's
// 33 ms tick, is 33 drops a step over the 128 yd window; scaled here by the intensity over the gate and by waterRainRipples. Snow: the same, a
// quarter as many and a quarter as deep, by waterSnowRipples (default 0). The field carries the rings from there.
constexpr float kFootprintHalfWeight = 0.5f;                 // VFogRippleStep Footprint() is 0.5 at about half the radius
constexpr float kRainGate = 0.2f;
constexpr float kRainDropsPerStep = 20.0f * (kWaterRippleTexels * kWaterRippleTexelYards) * (kWaterRippleTexels * kWaterRippleTexelYards) / (100.0f * 100.0f);
constexpr float kRainDropYards = 0.3f;
constexpr float kRainDropImpulse = -1.25f * 0.2f;      // Cata's weakest drop; the strongest is twice that
constexpr float kSnowShare = 0.25f;

// 0 while it is not raining (or snowing with waterSnowRipples on); share is the snow's quarter.
float DropStrength(float& share) {
    static CVar* s_rain = CVar::LookupRegistered("waterRainRipples");
    static CVar* s_snow = CVar::LookupRegistered("waterSnowRipples");
    const Weather* weather = World::s_weather;
    share = 1.0f;
    if (!weather || !(weather->m_intensity > kRainGate)) {
        return 0.0f;
    }
    if (weather->m_rain) {
        return s_rain ? std::clamp(s_rain->m_floatValue, 0.0f, 1.0f) : 1.0f;
    }
    share = kSnowShare;
    return weather->m_snow && s_snow ? std::clamp(s_snow->m_floatValue, 0.0f, 1.0f) : 0.0f;
}

uint32_t TakeDrops(const float centre[2], WaterRippleDisturbance* out, uint32_t capacity) {
    static uint32_t s_seed = 0x2545F491u;
    float share = 1.0f;
    const float strength = DropStrength(share);
    if (!(strength > 0.0f) || !capacity) {
        return 0;
    }
    const auto next = [] { s_seed = s_seed * 1664525u + 1013904223u; return static_cast<float>(s_seed >> 8) / 16777216.0f; };
    const float half = 0.5f * kWaterRippleTexels * kWaterRippleTexelYards;
    const float want = kRainDropsPerStep * share * std::min(World::s_weather->m_intensity, 1.0f);
    uint32_t count = std::min(capacity, static_cast<uint32_t>(want + next()));     // fractional counts carry as odds
    for (uint32_t i = 0; i < count; ++i) {
        WaterRippleDisturbance& d = out[i];
        d = {};
        d.from[0] = d.to[0] = centre[0] + (2.0f * next() - 1.0f) * half;
        d.from[1] = d.to[1] = centre[1] + (2.0f * next() - 1.0f) * half;
        d.radius = kRainDropYards / kFootprintHalfWeight;
        d.amplitude = kRainDropImpulse * (1.0f + next()) * strength * share;
    }
    return count;
}

// ---- simulation (coa water_ripples.cpp) ----------------------------------------------------------------------------
// One level of the field. The close level (units, rain) is coa's; the wide one (ships) is wxl-water's level 1.
struct Ripples {
    float yards = kWaterRippleTexelYards;
    double rate = kWaterRippleStepsPerSecond;
    CGxTex* maps[2] = {};
    int texels = 0;
    int current = 0;
    bool mapsCleared = false;
    int origin[2] = {};
    bool running = false;
    double epoch = 0.0;
    double lastScheduled = 0.0;
    int64_t stepsDone = 0;
    uint32_t quietSteps = 0;
    int maxSteps = kMaxWaterRippleStepsPerFrame;
};

struct RippleState {
    CGxShader* step = nullptr;
    CGxShader* splat = nullptr;         // VFogWakeSplat: the hulls into the wide level
    bool unsupported = false;
    Ripples close;
    Ripples wide = { kWideTexelYards, kWideStepsPerSecond };
    double wakeT0 = -1.0;               // the wakes' 1 Hz pulse starts at the first hull drawn (wxl-water ripT0_)

    WaterContactTracker contacts;
    double lastSeconds = -1.0;          // coa m_lastRippleSeconds
    int mapId = 0;
    double lastShaded = -1.0;           // client sprites (coa ClientRippleSprites m_lastShaded)
    bool mapsFailed = false;            // the last Prepare could not create the maps: keep the client sprites
};

RippleState s_ripples;

void Restart(Ripples& r) {
    r.running = false;
    r.mapsCleared = false;
    r.quietSteps = 0;
    r.current = 0;
}

void DestroyMaps(Ripples& r) {
    for (CGxTex*& map : r.maps) {
        if (map) {
            GxTexDestroy(map);
            map = nullptr;
        }
    }
    r.texels = 0;
    Restart(r);
}

bool PrepareShader() {
    CGxDevice* device = g_theGxDevicePtr;
    if (s_ripples.unsupported) {
        return false;
    }
    if (!device->Caps().m_rttFormat[GxTex_Gr1616F] || !Shader(s_ripples.step, "VFogRippleStep") || !Shader(s_ripples.splat, "VFogWakeSplat")) {
        s_ripples.unsupported = true;      // coa: "no filterable 16-bit floating-point render targets"
        return false;
    }
    return true;
}

bool Prepare(Ripples& r, int texels) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!PrepareShader()) {
        return false;
    }
    const bool close = &r == &s_ripples.close;
    if (r.maps[0] && r.maps[1] && r.texels == texels) {
        if (close) {
            s_ripples.mapsFailed = false;
        }
        return true;
    }
    DestroyMaps(r);
    if (close) {
        s_ripples.mapsFailed = true;
    }
    const EGxTexFormat format = GxTex_Gr1616F;
    for (CGxTex*& map : r.maps) {
        // Clamped and bilinear (coa: point for the step, which reads texel centres only; linear for the shading).
        CGxTexFlags flags(GxTex_Linear, false, false, 0, 0, true, 1, 0, 0, 0);
        device->TexCreate(GxTex_2d, texels, texels, 0, format, format, flags, nullptr, GxTexNullUserFunc, "VFogRipples", map);
        if (!map) {
            DestroyMaps(r);
            return false;
        }
    }
    r.texels = texels;
    if (close) {
        s_ripples.mapsFailed = false;
    }
    return true;
}

struct Schedule {
    int steps = 0;
    double stepSeconds[kMaxWaterRippleStepsPerFrame] = {};
};

Schedule ScheduleSteps(Ripples& r, double seconds) {
    Schedule schedule;
    if (!r.running || seconds < r.lastScheduled) {
        Restart(r);
        r.running = true;
        r.epoch = seconds;
        r.stepsDone = 0;
        r.lastScheduled = seconds;
        return schedule;
    }
    r.lastScheduled = seconds;
    const int64_t due = static_cast<int64_t>(std::floor((seconds - r.epoch) * r.rate)) - r.stepsDone;
    if (due <= 0) {
        return schedule;
    }
    const int64_t dropped = std::max<int64_t>(0, due - r.maxSteps);
    r.stepsDone += dropped;
    schedule.steps = static_cast<int>(due - dropped);
    for (int i = 0; i < schedule.steps; ++i) {
        schedule.stepSeconds[i] = r.epoch + static_cast<double>(r.stepsDone + 1 + i) / r.rate;
    }
    r.stepsDone += schedule.steps;
    return schedule;
}

int WindowOrigin(float centre, int texels, float yards) {
    return static_cast<int>(std::floor(static_cast<double>(centre) / yards)) - texels / 2;
}

bool OutsideWindow(const float from[2], const float to[2], float radius, int texels) {
    for (int axis = 0; axis < 2; ++axis) {
        if (std::max(from[axis], to[axis]) < -radius || std::min(from[axis], to[axis]) > texels + radius) {
            return true;
        }
    }
    return false;
}

void DrawStep(CGxTex* target, CGxTex* source, float keep) {
    const float keepRow[4] = { keep, 0.0f, 0.0f, 0.0f };
    SetPs(kKeepRegister, keepRow, 1);
    Bind(0, source);
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_ripples.step);
    DrawQuad(target->m_width, target->m_height);
    Bind(0, nullptr);
}

// wxl-water SplatHulls: each hull's polygon fanned from its centroid, added into the live map (r) before the step, in
// the window the map holds (its origin before this step's recentre). Every hull pulses together at 1 Hz.
uint32_t SplatHulls(const Ripples& r, const HullFill* hulls, uint32_t count, double seconds) {
    static C3Vector s_pos[1 + kHullPoints];
    static C2Vector s_tex[1 + kHullPoints];
    static uint16_t s_indices[3 * kHullPoints];
    uint32_t drawn = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const HullFill& h = hulls[k];
        if (s_ripples.wakeT0 < 0.0 || seconds < s_ripples.wakeT0) {
            s_ripples.wakeT0 = seconds;
        }
        const float s = std::clamp(kWideSplat * h.strength, -1.0f, 1.0f)
                      * static_cast<float>(std::sin(6.283185307179586 * std::fmod(seconds - s_ripples.wakeT0, kWakePulseSeconds) / kWakePulseSeconds));
        if (std::fabs(s) < 0.001f || h.n < 3) {
            continue;
        }
        if (!drawn) {
            C44Matrix projection;
            GxuXformCreateOrtho(0.0f, static_cast<float>(r.texels), 0.0f, static_cast<float>(r.texels), -1.0f, 1.0f, projection);
            GxXformSetProjection(projection);
            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRenderTargetSet(GxBuffers_Color, r.maps[r.current], 0);
            GxRsSet(GxRs_BlendingMode, GxBlend_NoAlphaAdd);     // ONE / ONE: g gains 0
            g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_ripples.splat);
        }
        // Window texels to DrawQuad's space: x across, y up from the bottom row, less the half texel where the backend
        // samples pixels at integer positions (wxl-water's - 0.5 on D3D9).
        const float centre = g_theGxDevicePtr->Caps().m_pixelCenterOnEdge ? 0.0f : 0.5f;
        float mx = 0.0f;
        float my = 0.0f;
        for (int i = 0; i < h.n; ++i) {
            const float u = static_cast<float>(h.x[i] / r.yards - r.origin[0]);
            const float v = static_cast<float>(h.y[i] / r.yards - r.origin[1]);
            s_pos[1 + i] = C3Vector(u - centre, r.texels - v + centre, 0.0f);
            mx += s_pos[1 + i].x / h.n;
            my += s_pos[1 + i].y / h.n;
            s_indices[3 * i] = 0;
            s_indices[3 * i + 1] = static_cast<uint16_t>(1 + i);
            s_indices[3 * i + 2] = static_cast<uint16_t>(1 + (i + 1) % h.n);
        }
        s_pos[0] = C3Vector(mx, my, 0.0f);
        const float splat[4] = { s * kPerturbCentre, 0.0f, 0.0f, 0.0f };
        SetPs(0, splat, 1);
        CImVector white(255, 255, 255, 255);
        GxPrimVertexPtr(1 + h.n, s_pos, sizeof(C3Vector), nullptr, 0, &white, 0, s_tex, sizeof(C2Vector), nullptr, 0);
        GxPrimIndexPtr(3 * h.n, s_indices);
        CGxBatch batch = { GxPrim_Triangles, 0, static_cast<uint32_t>(3 * h.n), 0, static_cast<uint16_t>(h.n) };
        g_theGxDevicePtr->Draw(&batch, 1);
        ++drawn;
    }
    if (drawn) {
        GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    }
    return drawn;
}

// coa WaterRipples::Step: recentre on the camera target (clearing both maps on a jump), upload the disturbances in
// window texels, one propagation into the other map. The wide level draws its hulls into the live map first
// (wxl-water StepLevel: SplatHulls, then the propagate through the window's whole-texel move).
void Step(Ripples& r, const float centre[2], const WaterRippleDisturbance* disturbances, uint32_t count, const HullFill* hulls = nullptr,
          uint32_t hullCount = 0, double seconds = 0.0) {
    const bool usable = std::fabs(centre[0]) <= kMaxWindowCentreYards && std::fabs(centre[1]) <= kMaxWindowCentreYards;
    const int desired[2] = { usable ? WindowOrigin(centre[0], r.texels, r.yards) : r.origin[0], usable ? WindowOrigin(centre[1], r.texels, r.yards) : r.origin[1] };
    int shift[2] = { desired[0] - r.origin[0], desired[1] - r.origin[1] };
    if (!r.mapsCleared || std::abs(shift[0]) >= r.texels || std::abs(shift[1]) >= r.texels) {
        const float clearRow[4] = { 0.0f, 0.0f, 1.0f / r.texels, 0.0f };     // no disturbances
        SetPs(kStepRegister, clearRow, 1);
        DrawStep(r.maps[0], r.maps[1], 0.0f);
        DrawStep(r.maps[1], r.maps[0], 0.0f);
        r.mapsCleared = true;
        shift[0] = shift[1] = 0;
        r.origin[0] = desired[0];
        r.origin[1] = desired[1];
    }
    const uint32_t splatted = hullCount ? SplatHulls(r, hulls, hullCount, seconds) : 0;
    r.origin[0] = desired[0];
    r.origin[1] = desired[1];

    static float s_rows[1 + 2 * kMaxWaterRippleDisturbances][4];
    float (*segments)[4] = &s_rows[kFirstSegmentRegister];
    float (*shapes)[4] = &s_rows[kFirstShapeRegister];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count && used < kMaxWaterRippleDisturbances; ++i) {
        const WaterRippleDisturbance& d = disturbances[i];
        const float radius = d.radius / r.yards;
        if (!(radius > 0.0f) || !std::isfinite(d.amplitude) || (d.amplitude == 0.0f && !d.held)) {
            continue;
        }
        float from[2];
        float to[2];
        for (int axis = 0; axis < 2; ++axis) {
            from[axis] = static_cast<float>(d.from[axis] / r.yards - r.origin[axis]);
            to[axis] = static_cast<float>(d.to[axis] / r.yards - r.origin[axis]);
        }
        if (OutsideWindow(from, to, radius, r.texels)) {
            continue;
        }
        segments[used][0] = from[0];
        segments[used][1] = from[1];
        segments[used][2] = to[0];
        segments[used][3] = to[1];
        shapes[used][0] = 1.0f / radius;
        shapes[used][1] = d.amplitude;
        shapes[used][2] = d.held ? kHeldFootprint : kAddedImpulse;
        shapes[used][3] = 0.0f;
        ++used;
    }
    s_rows[kStepRegister][0] = static_cast<float>(shift[0]);
    s_rows[kStepRegister][1] = static_cast<float>(shift[1]);
    s_rows[kStepRegister][2] = 1.0f / r.texels;
    s_rows[kStepRegister][3] = static_cast<float>(used);
    SetPs(kStepRegister, s_rows[kStepRegister], 1);
    if (used) {
        SetPs(kFirstSegmentRegister, segments[0], used);
        SetPs(kFirstShapeRegister, shapes[0], used);
    }
    DrawStep(r.maps[1 - r.current], r.maps[r.current], 1.0f);
    r.current = 1 - r.current;
    r.quietSteps = used || splatted ? 0 : r.quietSteps + 1;
    if (r.quietSteps >= kWaterRippleQuietSteps) {
        Restart(r);
    }
}

void Release() {
    DestroyMaps(s_ripples.close);
    DestroyMaps(s_ripples.wide);
    s_hullCache.clear();
    s_hullTracks.clear();
    s_ripples.contacts.Reset();
    s_ripples.lastSeconds = -1.0;
}

}  // namespace

// coa WaterRenderer::SimulateRipples + FillRippleConstants, plus wxl-water's wide level for the ships.
void RippleFrame(const FrameInputs& in, float strength, int quality, double seconds, const WorldViewport& vp, RippleShading& out) {
    out = {};
    RippleState& s = s_ripples;
    Ripples& r = s.close;
    Ripples& wide = s.wide;
    s.lastShaded = seconds;     // the modern water draws this frame (coa g_waterShadedThisFrame)
    bool shaded = false;
    bool wideShaded = false;
    if (strength <= 0.0f) {
        Release();
    } else {
        if (s.lastSeconds >= 0.0 && (seconds < s.lastSeconds || seconds - s.lastSeconds > kRippleGapSeconds || in.mapId != s.mapId)) {
            Restart(r);
            Restart(wide);
            s.contacts.Reset();
        }
        s.lastSeconds = seconds;
        s.mapId = in.mapId;
        static WaterContactFrame s_frame;
        CaptureWaterContacts(in.camTarget, s_frame);
        s.contacts.Update(s_frame, seconds);
        static HullFill s_hulls[kMaxHulls];
        const uint32_t hulls = CaptureHulls(in.camTarget, seconds, s_hulls, kMaxHulls);
        const float centre[2] = { in.camTarget[0], in.camTarget[1] };
        float dropShare = 1.0f;
        const bool raining = DropStrength(dropShare) > 0.0f;
        static SavedPassState s_saved;
        bool passes = false;
        // The wide level: the ships, every 83 ms (wxl-water's level 1 at ripplespeed 0.6), the hulls drawn at each step.
        wide.maxSteps = kMaxWideStepsPerFrame;
        if ((wide.running || hulls) && Prepare(wide, kWideTexels)) {
            const Schedule schedule = ScheduleSteps(wide, seconds);
            for (int step = 0; step < schedule.steps; ++step) {
                if (!passes) {
                    BeginPasses(s_saved);
                    passes = true;
                }
                Step(wide, centre, nullptr, 0, s_hulls, hulls, seconds);
            }
            wideShaded = wide.running && wide.mapsCleared;
        }
        if ((r.running || s.contacts.Emitting() || raining) && Prepare(r, quality == 1 ? kWaterRippleTexelsLow : kWaterRippleTexels)) {
            if (!r.running) {
                s.contacts.PlaceFootprints(seconds);
            }
            const Schedule schedule = ScheduleSteps(r, seconds);
            WaterRippleDisturbance disturbances[kMaxWaterRippleDisturbances];
            for (int step = 0; step < schedule.steps; ++step) {
                if (!passes) {
                    BeginPasses(s_saved);
                    passes = true;
                }
                uint32_t count = s.contacts.TakeDisturbances(schedule.stepSeconds[step], disturbances, kMaxWaterRippleDisturbances);
                count += TakeDrops(centre, disturbances + count, kMaxWaterRippleDisturbances - count);
                Step(r, centre, disturbances, count);
            }
            shaded = r.running && r.mapsCleared;
        }
        if (passes) {
            GxRenderTargetSet(GxBuffers_Color, vp.target, 0);
            EndPasses(s_saved, vp);
        }
    }
    // coa WaterRipples::Window / FillRippleConstants
    const float extent = r.texels > 0 ? r.texels * kWaterRippleTexelYards : 1.0f;
    const double progress = (seconds - r.epoch) * kWaterRippleStepsPerSecond - static_cast<double>(r.stepsDone);
    out.map = shaded ? r.maps[r.current] : nullptr;
    out.window[0] = static_cast<float>(r.origin[0] * static_cast<double>(kWaterRippleTexelYards));
    out.window[1] = static_cast<float>(r.origin[1] * static_cast<double>(kWaterRippleTexelYards));
    out.window[2] = 1.0f / extent;
    out.window[3] = r.texels > 0 ? 1.0f / r.texels : 0.0f;
    out.shape[0] = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    out.shape[1] = shaded ? strength : 0.0f;
    out.shape[2] = extent;
    out.fade[0] = extent * (0.5f - kRippleEdgeRampPerExtent);
    out.fade[1] = 1.0f / kRippleFadeYards;
    out.fade[2] = kWaterRippleTexelYards;
    // wxl-water's per-draw constants for level 1 (c38 / c39): the window of the last step, the step fraction.
    const double wideProgress = (seconds - wide.epoch) * kWideStepsPerSecond - static_cast<double>(wide.stepsDone);
    out.wideMap = wideShaded ? wide.maps[wide.current] : nullptr;
    out.wideWindow[0] = static_cast<float>(wide.origin[0] * static_cast<double>(kWideTexelYards));
    out.wideWindow[1] = static_cast<float>(wide.origin[1] * static_cast<double>(kWideTexelYards));
    out.wideWindow[2] = 1.0f / (kWideTexels * kWideTexelYards);
    out.wideWindow[3] = 1.0f / kWideTexels;
    out.wideShape[0] = static_cast<float>(std::clamp(wideProgress, 0.0, 1.0));
    out.wideShape[1] = wideShaded ? strength : 0.0f;
}

// coa RingsReplaceClientSprites + ClientRippleSprites::Update: while the modern water and its ripples run, plus 1 s.
bool RipplesHideClientSprites(bool allowed) {
    const double now = FrameTime::s_curTimeMs * 0.001;
    return allowed && !s_ripples.unsupported && !s_ripples.mapsFailed && s_ripples.lastShaded >= 0.0 && now >= s_ripples.lastShaded
        && now - s_ripples.lastShaded <= kClientSpriteGraceSeconds;
}

void RippleRelease() {
    Release();
}

void RippleShutdown() {
    Release();
    for (CGxShader* shader : { s_ripples.step, s_ripples.splat }) {
        if (shader && g_theGxDevicePtr) {
            g_theGxDevicePtr->ShaderDestroy(shader);
        }
    }
    s_ripples = RippleState{};
}

}  // namespace VolumetricFog
