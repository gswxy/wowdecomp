#ifndef WOW_SOURCE_WORLDCLIENT_FARTREESBAKE_H
#define WOW_SOURCE_WORLDCLIENT_FARTREESBAKE_H

// FARTREES: not in 12340. The worker side of FarTrees.cpp: one background thread that reads ADTs and tree models
// through the client's own file layer (SFile, so server patch MPQs win) and bakes each model's impostor atlas,
// cached on disk. Nothing here touches the Gx device; the main thread only uploads finished results.

#include <cstdint>
#include <string>
#include <vector>

namespace FarTreesBake {

constexpr uint32_t kFrames = 8;   // hemi-octahedral grid, kFrames x kFrames views

struct Place {
    std::string model;     // M2 path as the ADT names it (.mdx -> .m2)
    float x, y, z;         // world
    float yaw;             // 0..1 turns
    float scale;
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
    float centre[3] = {}, radius = 0, extent = 0;   // model space; extent = largest header-box side
    uint32_t framePx = 0;                           // one view's side; the atlas is kFrames x framePx square
    std::vector<std::vector<uint8_t>> albedo, normal;   // DXT5 mip levels, level 0 first, down to 1 x 1
};

void Start();
void Stop();   // joins the thread, drops queued work
void RequestTile(const std::string& map, int32_t tx, int32_t ty);
void RequestModel(const std::string& model);
bool PopTile(TileResult& out);
bool PopModel(ModelResult& out);

} // namespace FarTreesBake

#endif
