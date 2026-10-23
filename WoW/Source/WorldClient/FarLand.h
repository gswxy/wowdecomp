#ifndef WOW_SOURCE_WORLDCLIENT_FARLAND_H
#define WOW_SOURCE_WORLDCLIENT_FARLAND_H

// FARLAND: not in 12340. The WDL horizon (CMap::RenderLowDetail) drawn with each tile's real ground colour and a
// normal map, lit by the terrain's sun, on the stock WDL mesh and buffers. A worker thread bakes a tile the first
// time it is drawn (FarLandBake.cpp, from the ADT read through SFile, so the server's patch MPQs win) and caches it
// in Cache\FarLand\<map>\<col>_<row>.flt keyed by the ADT's size + CRC32; the main thread only copies finished
// tiles into two window atlases (farLandUploads a frame) and cross-fades them in from the stock fog colour. No fog
// change: under vfog its composite fogs the horizon band; without vfog the stock fog curve applies (fully fogged
// past farclip, the stock look). Plan and why the 2026-10-17 far-field stack was dropped: FARLAND-PLAN.md.

class CMapAreaLow;

namespace FarLand {

// CVars farLand (0 = stock WDL horizon, the default until approved; 1 = on) and farLandUploads.
void RegisterCVars();

// Start of CWorldScene::RenderHorizon: new map check, finished tiles into the atlases (capped), and the next Apply
// uploads this pass's view, sun and fog constants.
void BeginPass();

// Inside CMap::RenderLowDetail's RsPush, after its stock state: binds the far-land shaders, atlases and this
// tile's slot, or queues the tile's bake. false = draw the stock fog-coloured tile (farLand 0, not baked yet, no ADT).
bool Apply(const CMapAreaLow* area);

} // namespace FarLand

#endif
