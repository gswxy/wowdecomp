#ifndef WOW_SOURCE_WORLDCLIENT_FARLANDBAKE_H
#define WOW_SOURCE_WORLDCLIENT_FARLANDBAKE_H

// FARLAND: not in 12340. farLand's tile bake (FarLand.cpp runs it on its worker thread). Self-contained: no engine
// headers, so Tools/FarLand/bake_harness.cpp builds it natively to measure it. Per ADT tile:
//  - albedo 128 x 128 (4.17 yd a texel, one MCLY texture repeat): each layer texture's mean (its 4 x 4 mip),
//    blended by MCAL (sized by the MCNK's sizeAlpha, never the MCAL header), big alpha = weighted sum, else layered
//    lerp, x MCCV x 2; liquid cells (MH2O) in a flat colour per LiquidType type. DXT1, levels 128..4.
//  - normals 64 x 64 (8.3 yd) from MCVT heights, central differences; liquids point up. DXT5nm (x in alpha,
//    y in green), levels 64..4.
// Texel (u, v): u along the tile's columns (world -Y), v along its rows (world -X), cell centred.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace FarLandBake {

constexpr uint32_t kAlbedoPx = 128;
constexpr uint32_t kAlbedoLevels = 6;   // 128 .. 4
constexpr uint32_t kNormalPx = 64;
constexpr uint32_t kNormalLevels = 5;   // 64 .. 4
constexpr size_t kAlbedoBytes = 10920;  // DXT1, 8 bytes a block, all levels
constexpr size_t kNormalBytes = 5456;   // DXT5, 16 bytes a block, all levels

struct Tile {
    uint8_t albedo[kAlbedoBytes];
    uint8_t normal[kNormalBytes];
    uint8_t mean[3];   // the tile's mean colour (rgb), for the atlas levels where a texel spans tiles
};

// Reads a client file whole (the worker passes SFile, the harness StormLib). false = missing.
using ReadFn = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

struct Context {
    ReadFn read;
    bool bigAlpha = false;                              // WDT MPHD flag 0x4
    std::vector<uint8_t> liquidCategory;                // LiquidType id -> type (0 water, 1 ocean, 2 magma, 3 slime)
    std::unordered_map<std::string, uint32_t> texMean;  // layer texture -> 0xRRGGBB (cache across tiles)
};

uint32_t Crc32(const uint8_t* data, size_t size);
bool ReadBigAlpha(const std::vector<uint8_t>& wdt);
bool Bake(Context& ctx, const uint8_t* adt, size_t size, Tile& out);

// Byte offset of a level inside Tile::albedo / Tile::normal.
size_t AlbedoLevelOffset(uint32_t level);
size_t NormalLevelOffset(uint32_t level);

// One solid DXT1 block of rgb.
void SolidDxt1(const uint8_t rgb[3], uint8_t out[8]);

} // namespace FarLandBake

#endif
