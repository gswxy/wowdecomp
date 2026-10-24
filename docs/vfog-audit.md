# Volumetric fog audit (vfogaudit, 2026-10-24)

Scope: the volumetric fog of Wow64.exe / Wow32.exe on D3D9, D3D11 and Vulkan
(`WoW/Source/WorldClient/VolumetricFog*.cpp`, `Tools/CustomMpq/ShaderSrc/VFog*.hlsl` + `vf_*.hlsli`), its options
page, and where it meets `DayNight::UpdateFog`, `FarLand` and `WorldLighting`. The reference look is the user's CoA
config (`/root/_buggered/CoAVolFog(2).ini`). The reference code is `/root/wotlk-renderers/coa-vfog` and `/root/wxl-vfog`.

Backends: the renderer has no per-API branch that changes the look. Features switch on from device caps, so every
CVar below works the same on D3D9, D3D11 and Vulkan. Exceptions: the D3D11 SampleCmp shadow path (same result), and
`fogScattering`, which only exists on D3D11/Vulkan.

## What was messy

- **31 fog controls across four menu pages** (Fog, Light & God Rays, Colour Grading, Debug), plus `farFogReach` on World Effects.
- **On/off gates that repeat a strength of 0:** `vfogGrade` with `vfogGradeStrength`, `vfogShafts` with
  `vfogShaftIntensity`, `vfogLocalLights` with `vfogLocalLightIntensity`.
- **Four distance knobs:**
  - `vfogFarFog` (layer density)
  - `vfogStockFog` (half-off state)
  - `farFogReach` (fog-end stretch)
  - `vfogMaxDistance` (march range)

  `vfogFarFog` defaulted to **0**, so the fog had no distance layer at all. That 0 came from a coa preset made for
  wxl-farfield, which owned the far fog; our port has no far-field plugin. Together with `farFogReach` 1 (fog end
  stretched to 6 x farclip), it is why the far land showed its texture far too clearly.
- **Settings that are dead or redundant:**
  - `fogScattering` does nothing while vfog draws: the stock fog it tints is pushed out of range. It also duplicates `vfogSunScatter`.
  - `vfogLiquidDepth` does nothing while modern water draws.
  - `vfogNoiseScale` and `vfogNoiseWind` do nothing at `vfogNoise` 0 (the old default).
  - `vfogInteriorFarFog` does nothing at its default of 1.
- **One brightness split by data source:** `vfogExposure` applies on derived maps, `vfogClassicExposure` on maps
  with authored fog data. Exactly one of them applies per frame.

## CVars, before and after

"Menu" = shown on Video Options > Advanced. Everything not in the menu keeps its CVar (console / Config.wtf).

| CVar | before | after | menu after | note |
|---|---|---|---|---|
| vfog | 1 | 1 | Fog | master switch |
| vfogQuality | 1 | **2** | Fog | ini 3; 2 is the adapted default (our march also runs shadow taps per step, which coa did not) |
| vfogDensity | 0.8 | **1** | Fog (0..3) | ini 1; now the one density control (it scales every layer) |
| vfogNoise | 0 | **0.15** | Fog ("Patchiness") | ini NoiseAmount 0.15 |
| vfogSunScatter | 0.1 | 0.1 | Fog (0..1) | kept at the user's 0.1 (ini 1.0, same scale; the user found it too strong) |
| vfogGodRays | 1 | 1 | Fog | ini 0; the user chose 1 in our client |
| vfogShadowedSun | 1 | 1 | Fog | |
| vfogShaftIntensity | 0.35 | 0.35 | Fog | 0 = off (vfogShafts folded in) |
| vfogLocalLightIntensity | 1.2 | **1** | Fog (0..4) | ini 1.0; 0 = off (vfogLocalLights folded in) |
| vfogInteriorDensity | 0.15 | 0.15 | Fog ("Indoor fog") | |
| vfogUnderwater | 0 | 0 | Fog | |
| vfogGradeStrength | 0.5 | **0** | Fog ("Zone colour grading") | ini ColorGrading 0; 0 = off (vfogGrade folded in) |
| vfogFarFog | 0 | **1** | hidden | ini 1; the distance layer (aerial perspective) is back |
| vfogAmbient | 0.8 | **1** | hidden | ini 1 |
| vfogClassicExposure | 0.85 | **1** | hidden | ini 1 |
| vfogLocalLightPhase | 0.4 | **0.3** | hidden | ini 0.3 |
| vfogHaze, vfogGroundFog | 1, 0.6 | same | hidden | derived maps only; vfogDensity covers them |
| vfogExposure | 1 | 1 | hidden | |
| vfogTemporal | 0.85 | 0.85 | hidden | |
| vfogData, vfogClassicNoise, vfogClassicPhase, vfogColorSpace | 1, 1, 0, 1 | same | hidden | |
| vfogNoiseScale, vfogNoiseWind | 0.025, 0.5 | same | hidden | |
| vfogStartOffset, vfogMaxDistance | 10, 5000 | same | hidden | |
| vfogInterior, vfogInteriorFarFog | 1, 1 | same | hidden | |
| vfogCanopy | 0.5 | 0.5 | hidden | |
| vfogForeverGlow, vfogGlowCompensation | 0, 1 | same | hidden | |
| vfogDither, vfogHdrTargets | 1, 1 | same | hidden | the store dither is already off on fp16; only the composite dithers (ALU only) |
| vfogStockFog, vfogTransparent, vfogFadeEarly, vfogFadeStencil, vfogMsaaSplit, vfogLiquidDepth, vfogLateGlow | 1, 0, 1, 1, 1, 1, 1 | same | hidden (were on Debug) | pipeline switches; keep the defaults |
| vfogDebug, vfogSunMarker | 0 | 0 | Debug | debug views stay |
| **vfogGrade, vfogShafts, vfogLocalLights** | 1 | **removed** | – | now "strength > 0" |
| **farFogReach** | 1 | **removed** | – | replaced by the aerial-perspective rule (below) |
| fogScattering | 0 | 0 | Lighting | ignored while vfog draws; the tooltip now says so |
| farLand, horizonFarclipScale | 1, 6 | same | World Effects | |

The user-facing set went from 31 fog controls to 12 on one Fog page. The Colour Grading page is gone, and the old
"Light & God Rays" page is now "Sun & Moon" with only the sun-arc settings.

Values saved in Config.wtf from before the release still apply. A user who moved a slider keeps that value.

## CoA ini mapping

The fog maths are line for line coa's (`fog_model.cpp` against `VolumetricFogModel.cpp`), so a key and the CVar of
the same name share a scale.

| ini key (value) | ours | note |
|---|---|---|
| Enable (1) | vfog 1 | |
| EngineHooks, Overlay, OverlayKey, LogLevel | – | dll-only, no equivalent |
| Quality (3) | vfogQuality 2 | adapted (see above) |
| Density (1) | vfogDensity 1 | |
| Haze (1.0) / GroundFog (0.6) | vfogHaze 1 / vfogGroundFog 0.6 | |
| FarFog (1.0) | vfogFarFog 1 | was 0 |
| NoiseAmount (0.15) / NoiseScale (0.025) / NoiseWindSpeed (0.5) | vfogNoise 0.15 / vfogNoiseScale / vfogNoiseWind | |
| ClassicNoise (1) | vfogClassicNoise 1 | |
| StockFog (1) | vfogStockFog 1 | |
| TransparentFog (0) | vfogTransparent 0 | |
| DataMode (1) | vfogData 1 | |
| ColorSpace (1) | vfogColorSpace 1 | |
| SunScatter (1.0) | vfogSunScatter 0.1 | the user's own choice for our client |
| Ambient (1.0) / Exposure (1.0) / ClassicExposure (1.0) | vfogAmbient 1 / vfogExposure 1 / vfogClassicExposure 1 | |
| ClassicPhase (0) | vfogClassicPhase 0 | |
| LocalLights (1) / LocalLightIntensity (1.0) / LocalLightPhase (0.3) | vfogLocalLightIntensity 1 (> 0 = on) / vfogLocalLightPhase 0.3 | |
| InteriorAware (1) / InteriorDensity (0.15) | vfogInterior 1 / vfogInteriorDensity 0.15 | |
| GodRays (0) | vfogGodRays 1 | the user chose 1 in our client |
| GlowCompensation (1) / ForeverGlow (0) | vfogGlowCompensation 1 / vfogForeverGlow 0 | |
| ColorGrading (0) | vfogGradeStrength 0 | one knob now, as in coa |
| FarClipMax (1583) | – | the client's own `farclip` |
| MaxDistance (5000) / Temporal (0.85) / Underwater (0) | vfogMaxDistance / vfogTemporal / vfogUnderwater | |
| LiquidDepth (1) | vfogLiquidDepth 1 | forced on by our water |
| Multisampling (0) | – | different meaning: MSAA is always allowed here; vfogMsaaSplit is the per-sample fog split |
| DebugView (0) / SunMarker (0) | vfogDebug / vfogSunMarker | |
| Water* keys | vfogWater* | water, out of scope |

## Look fixes

1. **Far land and aerial perspective** (the user's farview feedback).
   - The fog end with far land drawing is now `min(2.5, horizonFarclipScale) x farclip` (`FarLand::FogCap`,
     `kHazeReach`). It was the full 6 x reach (`farFogReach`, removed).
   - With vfog, the distance layer is back (vfogFarFog 1). Its optical depth reaches 2.3 at that end
     (`kFarLandOpticalDepth`), so about 10 % of the land still shows through.
   - Land transmittance against distance: 0.93 at 1 x farclip, 0.71 at 1.5 x, 0.37 at 2 x, 0.10 from 2.5 x out.
   - Without vfog, the stock linear fog uses the same 2.5 x end, and `FarLandPS` caps it at 0.85 (`kFogMax`).
   - In both cases tall land such as Hyjal stays a darker silhouette with a ridge line against the sky, instead of a
     flat wall. The colour is the zone's fog colour (Light.dbc) by day and night, lit by the zone's sun and ambient.
2. **Far land popping** (`CMapLowDetail::CullHorizonAreas`, `FarLand.cpp`).
   - The horizon blocks were culled by the stock skyline occlusion buffer and occlusion volumes. Those were tuned for
     fog-coloured blocks, and they dropped blocks that peek over near hills or sit at the screen edge.
   - With far land drawing, a block now draws whenever its box, padded by 32 yd, touches the horizon frustum. That
     frustum is the view's own, at horizonFarclipScale x farclip, and the block range is measured from the camera.
     The heights come from the WDL min/max, so tall blocks were never the problem.
   - Two smaller far-land faults were fixed:
     - Tiles beyond `kKeep` were requested, baked and dropped again every frame, which delayed the near tiles.
       They are no longer requested.
     - `kKeep` 16 let two kept tiles 32 apart fight over one atlas slot. It is now 15.
   - The list is rebuilt every frame (`CullSortTable`), on rotation as well as movement.
3. **Sun blowout.** The fp16 fog stores no longer clip the in-scatter per channel (`DitheredStore`), so the
   composite's soft highlight roll-off now handles the sun halo instead of a hue-shifted clip. coa stores fp16 unclamped too.
4. **Shafts stacking on the sun.** The shaft floor is now `max(phase, floor)` instead of `phase + floor`, so the sun
   peak stays at 1, not 1.35. It also applies whether or not a shadow is tested, so lit fog no longer changes
   brightness with the shadow quality, or on D3D11 without hardware PCF.
5. **Night in authored zones.** The authored layers now get the elevation- and moon-faded scatter that the derived and
   distance layers already had. coa passed the raw value, so these zones had about 3 x the moon glow and a jump at
   the sun/moon handover.
6. **Indoors.** The indoor fade was a binary group test, so a door changed density x0.15 and turned the sun off in
   one frame. It is now eased (`SmoothedInteriorBlend`, time constant about 0.25 s).
7. **fogScattering** is skipped while vfog draws (`WorldLighting.cpp`), so the fitted fog no longer gets the sun twice on D3D11/Vulkan.

### Checked, not changed

- **Banding and dither.**
  - The fog and ray targets are fp16 wherever the device allows, which is every current card on all three backends.
  - The march is jittered per pixel (IGN + golden-ratio frame step), and the temporal pass averages it.
  - The store dither is already off on fp16. `vfogDither` 1 remains only on the 8-bit composite, as ALU only: no
    pass, no fetch. This is not the per-pixel deband pass that cost frame rate in the wxl plugins.
  - It stays on but hidden. If banding reports return, check the dark night-sky gradients in the 8-bit back buffer first.
- **Colour source.** Every colour comes from the zone light (fog, diffuse, ambient, sun cloud colour). The only
  hard-coded colours are the debug markers.
- **Height fog.**
  - The reference height is the camera's (coa's). Below it the density is uniform, with no hard cut.
  - When flying, ground fog fills everything below you; this is coa behaviour, left as is.
  - Open: a terrain-relative reference.
- **March step waste.** Steps run to MaxFogDistance and break at the last layer's end. Clamping the march to that
  end overflowed the ps_3_0 temps in the VFogMarch*LN variants, so it was left out. With the distance layer back on,
  the waste is about 20 % instead of about half.
- **D3D11 `hdr 1`.** The composite assumes a 0..1 scene. With HDR on, D3D11 blends fog over an fp16 scene while
  Vulkan tonemaps first. Not default; open.
- **Kept:**
  - vfogSunScatter 0.1
  - `VolumetricFog::OnWorldUpscaled` (seam)
  - the 15 % nearest-depth upsample (`kNearestDepthRelative`)
  - the 9-tap open-sky² god-ray mask
  - the shadowcombine fix
  - farLand 1 / horizonFarclipScale 6
  - the 85 % horizon blend to sky (`horizonEnd`)
