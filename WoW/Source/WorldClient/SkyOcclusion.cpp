#include "WoW/Source/WorldClient/SkyOcclusion.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/WorldClient/MapDoodadDef.h"
#include "WoW/Source/WorldClient/World.h"

namespace SkyOcclusion {

namespace {

constexpr int kSize = 256;                  // texels a side
constexpr float kCell = 2.0f;               // yd a texel: +-256 yd (benilla 0.5 yd, +-512 yd; its 3x3 taps 2 yd apart)
constexpr float kSnap = 32.0f;              // benilla SKY_SNAP
constexpr float kNone = -30000.0f;          // open sky
constexpr float kRayUp = 300.0f;            // yd above the camera a ray starts
constexpr float kRayDown = 400.0f;          // and below it ends
constexpr double kBudgetMs = 1.0;           // rays a frame
constexpr uint32_t kIntersectFlags = 0x110; // WMO and doodad collision (LegionLights' 0x111 less the terrain: the
                                            // land is the receiver, never its own roof)
constexpr float kMaxFootprint = 48.0f;      // yd: a doodad box wider is not a roof (cliffs, whole-scene props)
constexpr float kMinHeight = 2.0f;          // yd: a box lower roofs nothing (rocks, bushes, ground clutter)
constexpr uint32_t kRasterMs = 500;         // how often the doodad tops are rasterised again
constexpr uint32_t kUploadMs = 250;

CVar* s_cvar;
CVar* s_floor;
std::unordered_set<const CMapDoodadDef*> s_doodads;
bool s_doodadsDirty = true;
std::vector<float> s_rays;      // collision tops, kNone open
std::vector<float> s_props;     // doodad box tops
std::vector<float> s_heights;   // max of the two: what the device uploads
int s_x0 = INT32_MIN, s_y0 = INT32_MIN;   // the window's lower corner in texels of the world grid
uint32_t s_next;
uint32_t s_serial;
uint32_t s_lastRaster, s_lastUpload;
bool s_dirty;

void Shift(std::vector<float>& v, int dx, int dy) {
    std::vector<float> out(v.size(), kNone);
    for (int j = 0; j < kSize; ++j) {
        const int sj = j + dy;
        if (sj < 0 || sj >= kSize) {
            continue;
        }
        for (int i = 0; i < kSize; ++i) {
            const int si = i + dx;
            if (si >= 0 && si < kSize) {
                out[j * kSize + i] = v[sj * kSize + si];
            }
        }
    }
    v.swap(out);
}

void Raster() {
    std::fill(s_props.begin(), s_props.end(), kNone);
    const float wx0 = s_x0 * kCell;
    const float wy0 = s_y0 * kCell;
    for (const CMapDoodadDef* def : s_doodads) {
        const CAaBox& b = def->m_bbox;
        if (!(b.t.z - b.b.z >= kMinHeight) || b.t.x - b.b.x > kMaxFootprint || b.t.y - b.b.y > kMaxFootprint) {
            continue;
        }
        const int i0 = std::max(0, static_cast<int>(std::floor((b.b.x - wx0) / kCell)));
        const int i1 = std::min(kSize - 1, static_cast<int>(std::floor((b.t.x - wx0) / kCell)));
        const int j0 = std::max(0, static_cast<int>(std::floor((b.b.y - wy0) / kCell)));
        const int j1 = std::min(kSize - 1, static_cast<int>(std::floor((b.t.y - wy0) / kCell)));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                float& t = s_props[j * kSize + i];
                t = std::max(t, b.t.z);
            }
        }
    }
}

}  // namespace

void RegisterCVars() {
    s_cvar = CVar::Register("skyOcclusion", "Ground and walls under trees, roofs, bridges and overhangs get less of the sky's light (a map of the world's tops around you, D3D11): 1 = on, 0 = off (default)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_floor = CVar::Register("skyOcclusionFloor", "The share of the light a point fully under cover keeps, 0..1 (benilla 0.8)", 1, "0.8", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool Enabled() {
    return s_cvar && s_cvar->m_intValue > 0;
}

void AddDoodad(const CMapDoodadDef* def) {
    s_doodads.insert(def);
    s_doodadsDirty = true;
}

void RemoveDoodad(const CMapDoodadDef* def) {
    s_doodadsDirty |= s_doodads.erase(def) != 0;
}

bool Update(const float camera[3], const float*& heights, uint32_t& serial, float row[4], float params[4], bool force) {
    if (!force && (!s_cvar || s_cvar->m_intValue <= 0)) {
        if (!s_rays.empty()) {
            s_rays = {};
            s_props = {};
            s_heights = {};
            s_x0 = s_y0 = INT32_MIN;
        }
        return false;
    }
    if (s_rays.empty()) {
        s_rays.assign(kSize * kSize, kNone);
        s_props.assign(kSize * kSize, kNone);
        s_heights.assign(kSize * kSize, kNone);
    }
    // re-centre on the snap grid: keep what overlaps
    const int per = static_cast<int>(kSnap / kCell);
    const int x0 = static_cast<int>(std::lround(camera[0] / kSnap)) * per - kSize / 2;
    const int y0 = static_cast<int>(std::lround(camera[1] / kSnap)) * per - kSize / 2;
    if (x0 != s_x0 || y0 != s_y0) {
        if (s_x0 == INT32_MIN || std::abs(x0 - s_x0) >= kSize || std::abs(y0 - s_y0) >= kSize) {
            std::fill(s_rays.begin(), s_rays.end(), kNone);
        } else {
            Shift(s_rays, x0 - s_x0, y0 - s_y0);
        }
        s_x0 = x0;
        s_y0 = y0;
        s_doodadsDirty = true;
        s_lastRaster = s_lastUpload = 0;     // the new window's heights go up with its row this frame
    }
    const uint32_t now = OsGetAsyncTimeMs();
    // the collision tops, round robin
    const auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < kSize * kSize; ++n) {
        if ((n & 15) == 15 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > kBudgetMs) {
            break;
        }
        const uint32_t t = s_next++ % (kSize * kSize);
        const float x = (s_x0 + static_cast<int>(t % kSize) + 0.5f) * kCell;
        const float y = (s_y0 + static_cast<int>(t / kSize) + 0.5f) * kCell;
        const C3Vector from(x, y, camera[2] + kRayUp);
        const C3Vector to(x, y, camera[2] - kRayDown);
        C3Vector hit(0.0f, 0.0f, 0.0f);
        float f = 1.0f;
        const float top = World::Intersect(from, to, &hit, &f, kIntersectFlags, nullptr) && std::isfinite(f) && f < 1.0f
                              ? from.z + (to.z - from.z) * std::clamp(f, 0.0f, 1.0f) : kNone;
        if (top != s_rays[t]) {
            s_rays[t] = top;
            s_dirty = true;
        }
    }
    if (s_doodadsDirty && (!s_lastRaster || now - s_lastRaster >= kRasterMs)) {
        Raster();
        s_doodadsDirty = false;
        s_lastRaster = now;
        s_dirty = true;
    }
    if (s_dirty && (!s_lastUpload || now - s_lastUpload >= kUploadMs)) {
        for (int t = 0; t < kSize * kSize; ++t) {
            s_heights[t] = std::max(s_rays[t], s_props[t]);
        }
        s_dirty = false;
        s_lastUpload = now;
        ++s_serial;
    }
    heights = s_heights.data();
    serial = s_serial;
    row[0] = s_x0 * kCell;
    row[1] = s_y0 * kCell;
    row[2] = 1.0f / kCell;
    row[3] = static_cast<float>(kSize);
    params[0] = s_floor && std::isfinite(s_floor->m_floatValue) ? std::clamp(s_floor->m_floatValue, 0.0f, 1.0f) : 0.8f;
    params[1] = 0.75f;  // benilla: lifted a little, so the receiver's own surface never counts as its roof
    params[2] = 0.6f;   // ours: yd of lift per yd from the texel (a 30 degree floor never roofs itself, 2 yd texels)
    params[3] = 0.0f;
    return true;
}

}  // namespace SkyOcclusion
