#ifndef WOW_SOURCE_WORLDCLIENT_FARLAND_H
#define WOW_SOURCE_WORLDCLIENT_FARLAND_H

// FARLAND: not in 12340. The WDL horizon (CMap::RenderLowDetail) drawn with the map's real ground colour and a
// normal map, both baked offline (Tools/FarLand/bake_farland.py -> custom.MPQ FarLand\<map>.flnd/_a.blp/_n.blp),
// lit by the terrain's sun, on the stock WDL mesh and buffers. No runtime baking, no extra draws, no fog change:
// under vfog its composite fogs the horizon band; without vfog the stock fog curve applies (fully fogged past
// farclip, i.e. the stock look). Plan and the reasons the 2026-10-17 far-field stack was dropped:
// FARLAND-PLAN.md.

class CMapAreaLow;

namespace FarLand {

// CVar farLand (0 = stock WDL horizon, the default until approved; 1 = baked far land).
void RegisterCVars();

// Start of CWorldScene::RenderHorizon: the next Apply uploads this pass's view, sun and fog constants.
void BeginPass();

// Inside CMap::RenderLowDetail's RsPush, after its stock state: binds the far-land shaders, atlases and this
// tile's slot. false = draw the stock fog-coloured tile (farLand 0, no bake for this map, atlases still loading,
// or the tile is not in the bake).
bool Apply(const CMapAreaLow* area);

} // namespace FarLand

#endif
