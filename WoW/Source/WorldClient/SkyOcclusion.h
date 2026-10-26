#pragma once

#include <cstdint>

class CMapDoodadDef;

// BENILLA sky occlusion (benilla-bourbon modern/shadow.rs P3, sunshadow.wgsl sky_ambient; not in 12340): the open-sky
// fraction over a point from a top-down map of the static world's tops around the camera, D3D11 lighting post pass.
// The map: kSize^2 texels of kCell yd, re-centred on a 32 yd grid; each texel the highest of a straight-down ray
// through the world's WMO and doodad collision (World::Intersect, round robin under a ms budget a frame, so tiles
// that stream in are picked up) and the tops of the placed doodads' boxes over their footprints (tree crowns and props
// carry no roof collision).
namespace SkyOcclusion {

void RegisterCVars();
bool Enabled();             // skyOcclusion on
void AddDoodad(const CMapDoodadDef* def);
void RemoveDoodad(const CMapDoodadDef* def);

// Steps the map for the camera. False with skyOcclusion off. heights: kSize^2 world z (row = y texel), serial moves
// when they changed; row = (x0, y0, 1 / cell, size) world; params = (floor, lift, 0, 0).
bool Update(const float camera[3], const float*& heights, uint32_t& serial, float row[4], float params[4]);

}  // namespace SkyOcclusion
