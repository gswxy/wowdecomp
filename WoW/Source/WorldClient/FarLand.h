#ifndef WOW_SOURCE_WORLDCLIENT_FARLAND_H
#define WOW_SOURCE_WORLDCLIENT_FARLAND_H

// FARLAND: not in 12340. The WDL horizon (CMap::RenderLowDetail) drawn with each tile's real ground colour and a
// normal map, lit by the terrain's sun, on the stock WDL mesh and buffers. A worker thread bakes a tile the first
// time it is drawn (FarLandBake.cpp, from the ADT read through SFile, so the server's patch MPQs win) and caches it
// in Cache\FarLand\<map>\<col>_<row>.flt keyed by the ADT's size + CRC32; the main thread only copies finished
// tiles into two window atlases (farLandUploads a frame) and cross-fades them in from the stock fog colour. Fog: as
// wxl-farfield's game_fog, the fog end is stretched from the view distance out to the far land's reach
// (vfogaudit: 2.5 x farclip, aerial perspective, FarLand.cpp kHazeReach), so the land fades into the haze instead of being fog colour past
// farclip; FogCap feeds DayNight::UpdateFog (stock fog) and vfog (distance layer + horizon band blend). Plan and why the 2026-10-17 far-field stack was dropped: FARLAND-PLAN.md.

class CMapAreaLow;

namespace FarLand {

// CVars shadowFarReceivers (BindSunShadow), farLand (1 = on, the default since 2026-10-24 farview; 0 = stock WDL horizon), farLandUploads, farLandHazeHeight (farFogReach removed in vfogaudit).
void RegisterCVars();

// The fog end cap for a view distance: farClip, or with the far land drawing farClip stretched toward its reach.
float FogCap(float farClip);
// farLand is on and its shaders loaded: the horizon blocks show real land, so they must not pop (vfogaudit).
bool Drawing();
// The horizon pass's near plane (planar yd) for a camera far plane: stock farZ - 50, or with far land drawing pulled
// in to kHorizonNear so it fills any gap the near terrain leaves (farlandv2). BuildHorizonProjection and vfog's band decode.
float HorizonNear(float farZ);
// farLandHazeHeight as 1 / yd (0 = off or far land not drawing): the far haze thins with height above the camera.
float HazeFalloff();

// Start of CWorldScene::RenderHorizon: new map check, finished tiles into the atlases (capped), and the next Apply
// uploads this pass's view, sun and fog constants.
void BeginPass();

// Inside CMap::RenderLowDetail's RsPush, after its stock state: binds the far-land shaders, atlases and this
// tile's slot, or queues the tile's bake. false = draw the stock fog-coloured tile (farLand 0, not baked yet, no ADT).
bool Apply(const CMapAreaLow* area);

// Before StormDestroy (Client.cpp): stops and joins the bake worker, so it never reads SFile after the archives close.
void Shutdown();

// shadowFarReceivers (1 = on, default; 0 = stock): vf_shadow.hlsli's sun cascades for a pixel shader whose shadow
// lookup is camera-relative world (CWorldScene::s_activeWorldView): PS c56-c62 (c62.x = 0: off or no bands), c64
// gfxShadowFade, maps on s5/s6. FarLandPS, FarTreesPS and CataWaterAbove[P] read them.
void BindSunShadow();

} // namespace FarLand

#endif
