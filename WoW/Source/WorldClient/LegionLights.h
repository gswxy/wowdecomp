// LEGION lights: not in 12340 (Legion survey 2.8, backlog #7). The frame's M2 point lights on the surfaces around
// them, one screen-space pass from the scene depth (LegionLights.cpp, docs/LEGION_PORT.md "Legion lights").
#pragma once

class CMapDoodadDef;

namespace LegionLights {

void RegisterCVars();               // from CWorldParam::Initialize
// CGWorldFrame::OnWorldRender: false before scene->Draw(M2PASS_0) (lightMode 1: terrain and WMO only), true after
// the solid world (lightMode 2: models and grass too).
void OnWorld(bool solidDone);
// POINTLIGHT: true when this frame's pass lights terrain and WMOs (lightMode 1 or 2, the forward lightDynamic not
// owning the lights): their stock per-vertex point lights are then left out (M2Light.h g_m2SkipPointLights)
bool LightsWorldGeometry();
// LIGHTBAKE: a placed doodad's M2 point lights join the static light list (CMapDoodadDef::UpdateBounds), and leave
// it with the doodad (~CMapDoodadDef)
void AddDoodad(const CMapDoodadDef* def);
void RemoveDoodad(const CMapDoodadDef* def);

}  // namespace LegionLights
