#include "WoW/Source/WorldClient/ProbeGi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"

namespace ProbeGi {

namespace {

// benilla gi/mod.rs High tier: 32 x 16 x 32 probes, cascades 3 / 9 / 27 yd, 32 rays, reach 300 yd; ours 16 x 16 x 8
// (a CPU trace), so each cascade covers half benilla's span.
constexpr float kSpacing[kCascades] = { 3.0f, 9.0f, 27.0f };
constexpr int kRays = 32;
constexpr float kReach = 300.0f;
constexpr float kRayStart = 0.5f;          // tune.z: bias * 0.5 (High voxel 1 yd)
constexpr float kSunBias = 1.0f;           // tune.y
constexpr float kBounceGain = 1.0f;        // tune.w
constexpr float kSunReach = 200.0f;        // yd a hit's shadow ray looks toward the sun (benilla: the cascades)
constexpr float kBackfaceLimit = 0.35f;    // BACKFACE_LIMIT
constexpr float kSettleSecs = 19.8f;       // SETTLE_SECS
constexpr float kSkyGradient = 0.3f;       // SKY_GRADIENT
constexpr float kGroundAlbedo[3] = { 0.40f, 0.37f, 0.31f };   // GROUND_ALBEDO, every hit (no voxel albedo here)
constexpr float kNormalBias = 0.35f;       // NORMAL_BIAS
constexpr uint32_t kTerrain = 0x1, kWorld = 0x111;   // World::Intersect flags: terrain; terrain + WMO + doodads
constexpr uint32_t kUploadMs = 250;
constexpr int kProbes = kNx * kNy * kNz;

struct Probe {
    float dc[3], sh[3][3];  // E / pi = dc + sh . n
    float valid, sky, mean, meanSq;
    uint32_t updated;
    bool warm;
};

CVar* s_cvar;
CVar* s_strength;
CVar* s_budget;
std::vector<Probe> s_field[kCascades];
int s_base[kCascades][3];
bool s_placed[kCascades];
int s_cursor[kCascades];
int s_nextCascade;
uint32_t s_seed = 0x9E3779B9u;
std::vector<float> s_texels;
uint32_t s_serial, s_lastUpload;
bool s_dirty;
float s_ambient[3], s_diffuse[3], s_toSun[3];

float Rand() {
    s_seed = s_seed * 1664525u + 1013904223u;
    return (s_seed >> 8) * (1.0f / 16777216.0f);
}

// a uniform random rotation (Shoemake's quaternion), rows: benilla turns its rays every frame, we every probe update
void RandomRotation(float r[3][3]) {
    const float u1 = Rand(), u2 = Rand() * 6.2831853f, u3 = Rand() * 6.2831853f;
    const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
    const float x = a * std::sin(u2), y = a * std::cos(u2), z = b * std::sin(u3), w = b * std::cos(u3);
    const float m[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
        { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
        { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
    };
    std::memcpy(r, m, sizeof(m));
}

// gi_trace.wgsl fib_dir, z up
void FibDir(int i, float out[3]) {
    const float k = i + 0.5f;
    const float z = 1.0f - 2.0f * k / kRays;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const float phi = k * 2.399963f;
    out[0] = r * std::cos(phi);
    out[1] = r * std::sin(phi);
    out[2] = z;
}

bool Hit(const C3Vector& from, const C3Vector& to, uint32_t flags, float& t) {
    C3Vector hit(0.0f, 0.0f, 0.0f);
    t = 1.0f;
    return World::Intersect(from, to, &hit, &t, flags, nullptr) && std::isfinite(t) && t < 1.0f;
}

// probegi.wgsl nearest_index + gi_trace bounce_at: the field's irradiance at a point, the finest cascade holding it
void BounceAt(const float p[3], const float n[3], float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    for (int c = 0; c < kCascades; ++c) {
        if (!s_placed[c]) {
            continue;
        }
        int w[3];
        const int dims[3] = { kNx, kNy, kNz };
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            w[a] = static_cast<int>(std::lround(p[a] / kSpacing[c])) - s_base[c][a];
            inside &= w[a] >= 0 && w[a] < dims[a];
        }
        if (!inside) {
            continue;
        }
        const Probe& q = s_field[c][(w[2] * kNy + w[1]) * kNx + w[0]];
        if (q.valid <= 0.0f || !q.warm) {
            return;
        }
        for (int k = 0; k < 3; ++k) {
            out[k] = std::max(q.dc[k] + q.sh[0][k] * n[0] + q.sh[1][k] * n[1] + q.sh[2][k] * n[2], 0.0f) * kBounceGain;
        }
        return;
    }
}

// gi_trace.wgsl trace_rays + sum_probes for one probe, then gi_blend's ease
void TraceProbe(int c, int index, uint32_t now) {
    Probe& probe = s_field[c][index];
    const int i = index % kNx, j = (index / kNx) % kNy, k = index / (kNx * kNy);
    const C3Vector p((s_base[c][0] + i) * kSpacing[c], (s_base[c][1] + j) * kSpacing[c], (s_base[c][2] + k) * kSpacing[c]);
    float dc[3] = {}, sh[3][3] = {}, sky = 0.0f, back = 0.0f, dist = 0.0f, distSq = 0.0f;
    float t;
    // ours: a probe under the terrain (it lies over the probe) is outside the world (benilla: its backface share)
    const bool buried = Hit(C3Vector(p.x, p.y, p.z + 600.0f), p, kTerrain, t);
    if (!buried) {
        float rot[3][3];
        RandomRotation(rot);
        for (int r = 0; r < kRays; ++r) {
            float f[3], d[3];
            FibDir(r, f);
            for (int a = 0; a < 3; ++a) {
                d[a] = rot[a][0] * f[0] + rot[a][1] * f[1] + rot[a][2] * f[2];
            }
            float radiance[3];
            float hitDist = kReach;
            const C3Vector from(p.x + d[0] * kRayStart, p.y + d[1] * kRayStart, p.z + d[2] * kRayStart);
            const C3Vector to(p.x + d[0] * kReach, p.y + d[1] * kReach, p.z + d[2] * kReach);
            if (!Hit(from, to, kWorld, t)) {
                // probegi sky_radiance: the ambient row under a gradient whose cosine-weighted mean is 1
                const float g = 1.0f - kSkyGradient * (2.0f / 3.0f) + kSkyGradient * std::clamp(d[2], 0.0f, 1.0f);
                for (int a = 0; a < 3; ++a) radiance[a] = s_ambient[a] * g;
                sky += 1.0f;
            } else {
                hitDist = kRayStart + t * (kReach - kRayStart);
                // ours: no surface normal from the collision, the hit faces the ray; a hit closer than a tenth of a
                // spacing stands in for benilla's backface (the probe sits in a wall)
                const float n[3] = { -d[0], -d[1], -d[2] };
                back += hitDist < kSpacing[c] * 0.1f ? 1.0f : 0.0f;
                const float lift[3] = { p.x + d[0] * hitDist + n[0] * kSunBias, p.y + d[1] * hitDist + n[1] * kSunBias, p.z + d[2] * hitDist + n[2] * kSunBias };
                const float ndl = std::max(n[0] * s_toSun[0] + n[1] * s_toSun[1] + n[2] * s_toSun[2], 0.0f);
                float sun = 0.0f;
                if (ndl > 0.0f) {
                    const C3Vector l(lift[0], lift[1], lift[2]);
                    sun = Hit(l, C3Vector(l.x + s_toSun[0] * kSunReach, l.y + s_toSun[1] * kSunReach, l.z + s_toSun[2] * kSunReach), kWorld, t) ? 0.0f : ndl;
                }
                float bounce[3];
                BounceAt(lift, n, bounce);
                for (int a = 0; a < 3; ++a) radiance[a] = kGroundAlbedo[a] * (s_diffuse[a] * sun + bounce[a]);
            }
            for (int a = 0; a < 3; ++a) {
                dc[a] += radiance[a];
                for (int b = 0; b < 3; ++b) sh[b][a] += radiance[a] * d[b];
            }
            dist += hitDist;
            distSq += hitDist * hitDist;
        }
    }
    const float inv = 1.0f / kRays;
    const float share = back * inv;
    const float x = std::clamp((share - kBackfaceLimit * 0.7f) / (kBackfaceLimit * 0.6f), 0.0f, 1.0f);
    const float valid = buried ? 0.0f : 1.0f - x * x * (3.0f - 2.0f * x);
    const float h = probe.warm ? std::clamp(std::exp(-static_cast<float>(now - probe.updated) * 0.001f / kSettleSecs), 0.0f, 0.99f) : 0.0f;
    const auto ease = [h](float& v, float n) { v = n + (v - n) * h; };
    for (int a = 0; a < 3; ++a) {
        ease(probe.dc[a], dc[a] * inv);
        for (int b = 0; b < 3; ++b) ease(probe.sh[b][a], sh[b][a] * 2.0f * inv);
    }
    ease(probe.valid, valid);
    ease(probe.sky, sky * inv);
    ease(probe.mean, dist * inv);
    ease(probe.meanSq, distSq * inv);
    probe.updated = now;
    probe.warm = true;
    s_dirty = true;
}

// keeps the probes that stay inside a re-centred cascade, the rest go cold
void Place(int c, const float camera[3]) {
    const int dims[3] = { kNx, kNy, kNz };
    int base[3];
    for (int a = 0; a < 3; ++a) {
        base[a] = static_cast<int>(std::floor(camera[a] / kSpacing[c])) - dims[a] / 2;
    }
    if (s_placed[c] && !std::memcmp(base, s_base[c], sizeof(base))) {
        return;
    }
    std::vector<Probe> out(kProbes, Probe{});
    if (s_placed[c]) {
        for (int k = 0; k < kNz; ++k) for (int j = 0; j < kNy; ++j) for (int i = 0; i < kNx; ++i) {
            const int si = i + base[0] - s_base[c][0], sj = j + base[1] - s_base[c][1], sk = k + base[2] - s_base[c][2];
            if (si >= 0 && si < kNx && sj >= 0 && sj < kNy && sk >= 0 && sk < kNz) {
                out[(k * kNy + j) * kNx + i] = s_field[c][(sk * kNy + sj) * kNx + si];
            }
        }
    }
    s_field[c].swap(out);
    std::memcpy(s_base[c], base, sizeof(base));
    s_placed[c] = true;
    s_dirty = true;
}

void Pack() {
    s_texels.assign(static_cast<size_t>(kTexW) * kTexH * 4, 0.0f);
    for (int c = 0; c < kCascades; ++c) {
        for (int n = 0; n < kProbes; ++n) {
            const Probe& q = s_field[c][n];
            if (!q.warm) {
                continue;
            }
            const int row = c * kNy * kNz + n / kNx;   // (k * kNy + j) rows a cascade
            float* t = &s_texels[(static_cast<size_t>(row) * kTexW + (n % kNx) * kTexels) * 4];
            const float v[kTexels][4] = {
                { q.dc[0], q.dc[1], q.dc[2], q.valid },
                { q.sh[0][0], q.sh[0][1], q.sh[0][2], q.sky },
                { q.sh[1][0], q.sh[1][1], q.sh[1][2], 0.0f },
                { q.sh[2][0], q.sh[2][1], q.sh[2][2], 0.0f },
                { q.mean, q.meanSq, 0.0f, 0.0f },
            };
            std::memcpy(t, v, sizeof(v));
        }
    }
}

}  // namespace

void RegisterCVars() {
    s_cvar = CVar::Register("probeGi", "The light bounced off the world around you (a field of light probes traced through the world, D3D11): replaces the flat ambient light, darker under cover and tinted by the sunlit ground: 1 = on, 0 = off (default)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_strength = CVar::Register("probeGiStrength", "How much of the stock ambient light the probes replace, 0..1", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_budget = CVar::Register("probeGiBudget", "Milliseconds a frame the probe trace may take, 0.25..8 (more = the field settles faster)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool Enabled() {
    return s_cvar && s_cvar->m_intValue > 0;
}

bool Update(const float camera[3], const float*& probes, uint32_t& serial, float grid[kCascades][4], float params[4], float ambient[4]) {
    if (!Enabled()) {
        return false;
    }
    const DayNight::DNInfo* info = DayNight::GetInfo();
    if (!info) {
        return false;
    }
    const CImVector& am = info->m_bands.m_ambient;
    const CImVector& di = info->m_bands.m_diffuse;
    s_ambient[0] = am.r / 255.0f; s_ambient[1] = am.g / 255.0f; s_ambient[2] = am.b / 255.0f;
    s_diffuse[0] = di.r / 255.0f; s_diffuse[1] = di.g / 255.0f; s_diffuse[2] = di.b / 255.0f;
    const C3Vector& dir = info->m_light0.m_dir;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    s_toSun[0] = len > 0.0f ? -dir.x / len : 0.0f;
    s_toSun[1] = len > 0.0f ? -dir.y / len : 0.0f;
    s_toSun[2] = len > 0.0f ? -dir.z / len : 1.0f;

    for (int c = 0; c < kCascades; ++c) {
        Place(c, camera);
    }
    // the trace: one probe at a time, the cascades in turn, until the frame's budget is spent
    const uint32_t now = OsGetAsyncTimeMs();
    const double budget = std::clamp(s_budget ? s_budget->m_floatValue : 1.0f, 0.25f, 8.0f);
    const auto start = std::chrono::steady_clock::now();
    do {
        const int c = s_nextCascade;
        s_nextCascade = (s_nextCascade + 1) % kCascades;
        TraceProbe(c, s_cursor[c], now);
        s_cursor[c] = (s_cursor[c] + 1) % kProbes;
    } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < budget);

    if (s_texels.empty() || (s_dirty && now - s_lastUpload >= kUploadMs)) {
        Pack();
        s_dirty = false;
        s_lastUpload = now;
        ++s_serial;
    }
    probes = s_texels.data();
    serial = s_serial;
    for (int c = 0; c < kCascades; ++c) {
        grid[c][0] = s_base[c][0] * kSpacing[c];
        grid[c][1] = s_base[c][1] * kSpacing[c];
        grid[c][2] = s_base[c][2] * kSpacing[c];
        grid[c][3] = kSpacing[c];
    }
    params[0] = std::clamp(s_strength ? s_strength->m_floatValue : 1.0f, 0.0f, 1.0f);
    params[1] = kNormalBias;
    params[2] = 1.0f;   // chebyshev (High and above)
    params[3] = 0.0f;
    ambient[0] = s_ambient[0]; ambient[1] = s_ambient[1]; ambient[2] = s_ambient[2]; ambient[3] = 0.0f;
    return true;
}

}  // namespace ProbeGi
