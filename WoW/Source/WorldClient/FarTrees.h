#ifndef WOW_SOURCE_WORLDCLIENT_FARTREES_H
#define WOW_SOURCE_WORLDCLIENT_FARTREES_H

// FARTREES: not in 12340. Trees past the stock doodad fade (size classes 2 and 3: 4-100 yd, fading at ~200 / ~750 yd)
// drawn to farclip as hemi-octahedral impostors. Placements and atlases come from FarTreesBake's worker thread at run
// time (ADTs through SFile, atlases baked from the model and cached in Cache/FarTrees). One draw per (visible tile,
// model) from static buffers; FarTreesVS/PS pick and mix the 4 nearest baked views, light from the sun, fog, and
// write the baked depth.
// Design and the reasons the old cards (e658201 / 4d4185a) failed: fartrees/FARTREES-PLAN.md.

#include <cstdint>
#include <vector>

class CAaBox;
class CFrustum;
class C3Vector;

namespace FarTrees {

// CVar farTrees (WorldParam_CVar.cpp), default 0.
extern int32_t s_enabled;
// CVar farTreesShadow, default 1: the impostors cast the sun's shadow (only with farTrees).
extern int32_t s_shadow;

// CWorldScene::Render, after the WMO groups and before the horizon (main thread). Draws before every M2 pass, so a
// fading real tree blends over its own impostor.
void Draw();

// Stops the worker thread before the client closes the archives it reads (Client.cpp, no device calls).
void Shutdown();

// farTreesShadow, part 1: CShadowQuery::RenderCallback, cascade bands 0-2, after the stock casters. The trees past
// their doodad fade (the real model casts until then) as depth-only quads on the atlas view nearest the light,
// one draw per (tile, model). extents / cull: the band's (world); camera: what the band's view is relative to.
bool ShadowWanted();
void DrawShadow(const CAaBox& extents, CFrustum& cull, const C3Vector& camera);

// farTreesShadow, part 2: the trees as canopy bumps in TerrainSunShadow's lit heights (past the cascades).
struct Canopy { float x, y, top, radius; };
uint32_t CanopyGeneration();                  // changes whenever the set below does (a tile or a model arrived or left)
void Canopies(std::vector<Canopy>& out);     // every placed tree whose model is baked (empty when off)

} // namespace FarTrees

#endif
