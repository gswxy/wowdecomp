#ifndef WOW_SOURCE_WORLDCLIENT_FARTREES_H
#define WOW_SOURCE_WORLDCLIENT_FARTREES_H

// FARTREES: not in 12340. Trees past the stock doodad fade (size classes 2-4, placed 6 yd or taller) drawn to farclip
// as wxl-pack billboards (and past it on the far land: farTreesBeyond, DrawBeyond): a card of the model's frame,
// bottom pivot, standing along the placement's lean and facing the camera, showing the nearest of 4 baked side
// views. Placements and pictures come from FarTreesBake's worker thread at run time (ADTs through SFile, pictures
// baked from the model and cached in Cache/FarTrees). One draw per (visible tile, model) from static buffers.
// Design: fartrees/FARTREES-PLAN.md (section 9: the wxl port).

#include <cstdint>
#include <vector>

class CAaBox;
class CFrustum;
class C3Vector;

namespace FarTrees {

// CVar farTrees (WorldParam_CVar.cpp), default 1.
extern int32_t s_enabled;
// CVar farTreesShadow, default 1: the impostors cast the sun's shadow (only with farTrees).
extern int32_t s_shadow;
// CVar farTreesBeyond, default 1: with farLand drawing, trees also past farclip, in the horizon pass (DrawBeyond).
extern int32_t s_beyond;
// CVar farTreesReach (yd), default 0 = auto: the far land's haze end (FarLand::FogCap, 2.5 x farclip); capped at the
// horizon's far plane (horizonFarclipScale x farclip).
extern float s_reachSetting;

// CWorldScene::Render, after the WMO groups and before the horizon (main thread). Draws before every M2 pass, so a
// fading real tree blends over its own impostor.
void Draw();

// farTreesBeyond: CWorldScene::RenderHorizon, after the far land, with the horizon's projection, depth band and
// camera-relative world xform. The trees Draw left past the split, hazed as the far land (main view only).
void DrawBeyond();

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
