#ifndef WOW_SOURCE_WORLDCLIENT_FARTREESBAKE_H
#define WOW_SOURCE_WORLDCLIENT_FARTREESBAKE_H

// FARTREES: not in 12340. The worker side of FarTrees.cpp: one background thread that reads ADTs and tree models
// through the client's own file layer (SFile, so server patch MPQs win) and bakes each model's impostor atlas,
// cached on disk (a port of wraithhost-lite FarLod.cpp / Impostor.cpp). Nothing here touches the Gx device; the main thread only uploads finished results.

#include <cstdint>
#include <string>
#include <vector>

namespace FarTreesBake {

// wxl billboards (wraithhost-lite Impostor.hpp): kViews side views every 90 deg about the model's z, kPx a side at
// level 0, side by side in one kViews*kPx x kPx atlas; levels 0..kLevels-1 keep their coverage (the PS reads no
// coarser), the rest of the chain down to 1 x 1 is there for the device only.
constexpr uint32_t kViews = 4, kPx = 64, kLevels = 5;
constexpr uint32_t kAtlasW = kViews * kPx, kAtlasH = kPx, kChain = 9;   // 256 x 64 .. 1 x 1

struct Place {
    std::string model;     // M2 path: the first one seen with this file name (the game shares CM2Shared by leaf name)
    float x, y, z;         // world
    float yaw;             // 0..1 turns
    float rot[3];          // MDDF rotation as read (degrees), for the stock size class
    float scale;
    float size;            // header box: largest side (model units), for the tile bounds before the picture is in
    uint32_t uniqueId;
};

struct TileResult {
    std::string map;
    int32_t tx, ty;
    std::vector<Place> places;
};

struct ModelResult {
    std::string model;
    bool ok = false;
    bool fromCache = false;
    // Impostor.cpp: frame = max(zHi - zLo, 2 x widest reach from the z axis) over every vertex, bottom = zLo (model
    // space); the card is frame x scale a side, its bottom edge at the placement's z + bottom x scale.
    float frame = 0, bottom = 0, height = 0, reach = 0;
    float box[6] = {};                              // the M2 header's bounding box (model space)
    // DXT5, kChain levels of the kAtlasW x kAtlasH atlas, level 0 first. albedo: rgb + coverage; normal: the view's
    // normal x (right), y (up) as 0..1, blue = the unlit mask (M2 render flag 1).
    std::vector<std::vector<uint8_t>> albedo, normal;
};

void Start();
void Stop();   // joins the thread, drops queued work
void RequestTile(const std::string& map, int32_t tx, int32_t ty);
void CancelTile(const std::string& map, int32_t tx, int32_t ty);   // drops it if still queued
void RequestModel(const std::string& model);
bool PopTile(TileResult& out);
bool PopModel(ModelResult& out);

} // namespace FarTreesBake

#endif
