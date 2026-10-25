#ifndef WOW_SOURCE_WORLDCLIENT_TERRAINHEIGHTGEN_H
#define WOW_SOURCE_WORLDCLIENT_TERRAINHEIGHTGEN_H

// TERRAINPOM: not in 12340. The terrain parallax height map of one tileset texture, made at run time from the
// diffuse the client loaded (so a texture pack that replaces the tileset gets matching shapes), instead of a
// pre-baked `_h.blp`. A port of wxl-light tools/make_heightmaps.py (commit f4d6271) to plain C++, no engine deps
// (Tools/TerrainHeightGenCheck.py builds it natively and compares it against the Python).

#include <cstdint>
#include <vector>

namespace TerrainHeightGen {

enum Kind {
    Kind_Failed = -1,
    Kind_Soft = 0,          // gentle smoothed luminance relief (grass, dirt, sand, snow)
    Kind_Stones = 1,        // a dome per stone, dark joints
    Kind_StonesBright = 2,  // a dome per stone, bright mortar
};

// bgra: side x side texels, B G R A bytes, rows packed (side a power of two). name: the tileset path (picks the
// height range by its words). Out: heights 0..1, outSide x outSide (min(128, side)). Kind_Failed when side is no
// power of two.
Kind Generate(const uint8_t* bgra, uint32_t side, const char* name, std::vector<float>& out, uint32_t& outSide);

} // namespace TerrainHeightGen

#endif
