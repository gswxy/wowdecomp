#ifndef WOW_SOURCE_WORLDCLIENT_FARTREES_H
#define WOW_SOURCE_WORLDCLIENT_FARTREES_H

// FARTREES: not in 12340. Trees past the stock doodad fade (size classes 2 and 3: 4-100 yd, fading at ~200 / ~750 yd)
// drawn to farclip as hemi-octahedral impostors. Placements and atlases come from FarTreesBake's worker thread at run
// time (ADTs through SFile, atlases baked from the model and cached in Cache/FarTrees). One draw per (visible tile,
// model) from static buffers; FarTreesVS/PS pick and mix the 4 nearest baked views, light from the sun, fog, and
// write the baked depth.
// Design and the reasons the old cards (e658201 / 4d4185a) failed: fartrees/FARTREES-PLAN.md.

#include <cstdint>

namespace FarTrees {

// CVar farTrees (WorldParam_CVar.cpp), default 0.
extern int32_t s_enabled;

// CWorldScene::Render, after the WMO groups and before the horizon (main thread). Draws before every M2 pass, so a
// fading real tree blends over its own impostor.
void Draw();

} // namespace FarTrees

#endif
