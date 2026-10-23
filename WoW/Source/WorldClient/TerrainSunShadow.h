#ifndef WOW_SOURCE_WORLDCLIENT_TERRAINSUNSHADOW_H
#define WOW_SOURCE_WORLDCLIENT_TERRAINSUNSHADOW_H

class CGxTex;

// Not stock (gfxTerrainShadow): the sun blocked by the land, near ADT and far WDL alike, which the engine's shadow
// cascades do not cover (terrain does not cast into them, and they end at 640 yd). Built from the map's WDL heights
// (one height per 33.3 yd, the whole map) around the camera:
// - a "lit height" texture, 256 x 256 texels on the WDL vertices (+-4.2 km): the lowest world z the sun reaches at
//   each texel, swept toward the light; vf_shadow.hlsli tests the fog's march points against it;
// - shadowMaskFine: a nested fine level (clipmap ring) for the shadow mask, 512 x 512 texels on the loaded ADTs' MCVT
//   outer vertices (4.2 yd, +-1067 yd; WDL where no ADT is loaded, holes let light through), its light-side edge
//   seeded from the coarse level's lit heights;
// - a CPU ray test toward the light (the sun glare's point occlusion), on the fine heights inside their window.
namespace TerrainSunShadow {

void RegisterCVars();
bool Enabled();
bool MaskWanted();          // shadowMask on (and gfxTerrainShadow): the land's shadow over the world
float MaskSoftYards();      // shadowSoft: the mask's penumbra in yards under the lit height

// Rebuilds the texture when the light turned or the camera moved far enough. False (and no texture) with the CVar
// off, no WDL on the map, or the light under the horizon. row = (u0, v0, k, 1/soft) for uv = row.xy - row.z * world.yx.
bool Update(const float toLight[3], const float camPos[3], CGxTex*& tex, float row[4]);

// shadowMaskFine: the nested ADT level Update built (4.2 yd texels, +-1067 yd; same lit-height texels), for the shadow
// mask. row = (u0, v0, k, 1 / blend uv): the mask fades it into the coarse level near its edge. False (row 0): off.
bool Fine(CGxTex*& tex, float row[4]);

// 0 when the land between pos and the light (toward, unit) blocks it, else 1.
float Visibility(const float pos[3], const float toward[3]);

void Release();     // the texture (device lost or restored, shutdown)

}  // namespace TerrainSunShadow

#endif
