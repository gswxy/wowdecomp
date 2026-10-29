# 00 — Current shadow system (map, 2026-10-24)

Read-only survey of /root/wowdecomp HEAD (last shadow commit 388da4bb). Paths are repo-relative unless absolute.
Earlier audits (some since fixed): /root/d3d11-port/shadow-audit-{receivers,engine,fog-sun}.md and SHADOW_PORT_PLAN.md.

## 1. Pipeline overview

Frame order (WoW/Source/Ui/WorldFrame.cpp:1790-1830), each step in turn:
1. CShadowQuery::Update, inside CWorldScene::Render, after M2 animate
2. terrain
3. WMO
4. horizon and sky
5. M2 opaque (M2PASS_0)
6. detail doodads
7. **VolumetricFog::OnSolidDone = shadow mask** (:1806)
8. SSAO
9. M2PASS_1
10. weather
11. liquids
12. M2PASS_2

Liquids and the transparent passes draw after the mask.

### 1.1 Light direction
- `SunShadowDir` is at WoW/Source/WorldClient/ShadowQuery.cpp:204-275, under gfxSunShadows.
  - It takes the true sun or moon (`DayNight::g_lightBody`), with elevation clamped to 12-70° (:166).
  - The direction is latched in 0.01° steps scaled by sin², with a 0.004° minimum. Jumps over 5° are taken at once, and it waits at most 2 s (:170-172).
  - Each latch step sets `s_bandRedraw` (:263-270).
- Indoors uses the stock steep light. Indoors means `VolumetricFog::CameraInteriorBlend` ≥ 0.6, released below 0.4 (:208-212).
  - The light is the world light direction with z×5, clamped to ≥ -1.2 and renormalised (:336-348).
  - Switching between indoors and outdoors redraws every band (:215-217).
- The land heightfield uses the true, unclamped light instead (VolumetricFogRenderer.cpp:798, `in.toLight`).
- The mask's N·L uses `m_light1` (`shadeToLight`, :1485).

### 1.2 Shadow maps (Engine/Source/ShaderEffect/ShadowCache.cpp)
- **Modes** (:28-39):
  - 0 = none, 1-2 = unit map only, 3-4 = cached bands, 5 = per-frame cascades.
  - shadowValue = {0,1,1,2,2,3,3} (:39).
  - `ValidateShadowMode` (:291-355): D3D9 needs vs/ps 3.0 for modes 3-5. D3D10, D3D11 and Vulkan only need a depth-capable format. GLL is limited to ≤2, and OpenGL to 0.
- **Size and format** (`Init`, :1024-1036):
  - 1024, or 2048 for MEDIUM and HIGH+.
  - The `shadowMapSize` override applies **only with PCF** (:1033). It defaults to 4096 on _WIN64 and 0 on Wow32 (ShadowQuery.cpp:57-61, 63-78).
  - With PCF: D24X8 depth plus an ARGB dummy render target, linear filter.
  - Without PCF: an R32F colour render target, point filter, drawn using the screen's depth buffer (:1049-1058).
- **Unit map** `s_extDynShadow` (radius 20 yd, :1076):
  - Pass 1 draws query flags 3 (units; flag 2 = interior units below `s_cutoffHeight`) into `s_intDynShadow`. `s_cutoffHeight` is player z+2 (CWorldSceneRender.cpp:309).
  - Pass 2 runs only when the exterior is visible (`useCullFrustum`, ShadowQuery.cpp:333). It draws flags 1 (q≤2), 5 (q3-4) or 13 (q5) into `s_extDynShadow` (ShadowCache.cpp:1187-1220).
  - Both unit maps share the same matrix rows (`s_shadowMatrices[0..2]`, :821-835).
- **Bands** `s_extShadowBands[3]`, radius 40/160/640 yd (:22):
  - Square orthographic maps, near/far 1..4000 along the light, with the eye 2000 yd up-light of the centre (ShadowQuery.cpp:402-418, 465).
  - The centre is the viewer, meaning the camera *target*, not the camera (ShadowQuery.cpp:146-159).
  - gfxShadowAnchor keeps the lowest z over 1 s, with at most 24 yd of lag (:282-327).
  - gfxShadowSnap snaps to whole texels around a 256 yd reference (ShadowCache.cpp:111-137).
  - **No frustum-fitted splits.** The receiver picks the band: the nearest band where |uv| < 1.
  - **q3-4** (`GatherBands`, :576-676):
    - Double-buffered.
    - Redrawn when the viewer moves sqrt(4/16/1024) = 2/4/32 yd (the compare is squared, :592), or on a light latch.
    - The redraw is incremental, in 3×3 tiles (bands 0-1) or 5×5 tiles (band 2) over 9 or 25 frames, then flipped (:611-672).
    - Query flags **8 only** (:645).
  - **q5** (:677-765):
    - Single buffer, flags 13, drawn every frame.
    - With shadowCascadeReuse, band 1 redraws every 2nd frame and band 2 every 4th. They redraw sooner on a turn over 2°, a 2% radius move, or any light change (`CascadeDue`, :79-106). The cull box is widened by 5% (ShadowQuery.cpp:499-504).
    - Not drawn at all when no exterior is visible (:1227).
- **Bias** (`MatrixCallback`, ShadowQuery.cpp:426-452):
  - 0.5 (PCF)
  - plus a slope term: gfxShadowSlopeBias 1.5 × texel × cot(elev), capped at 3
  - plus a per-band constant
- **PCF kernel**: 8 fixed offsets × shadowSoftness (ShadowCache.cpp:407-446). Receivers take 5 taps.
- **Uploads to receivers:**
  - `SetShadowMapGenericGlobal` (:450-471): VS c224-235, PS c4 (sun view direction), PS c5-12 (PCF), samplers s5-7 (bands).
  - `SetShadowMapGenericInterior(interior)` (:475-506): s4 = interior or exterior unit map, PS c3 = cutoff plane faded by gfxShadowFade.
  - `SetShadowMapTerrain` (:510-536): VS c37, PS c3, PS c15 (fade), s5 (unit map), s6-8 (bands).
  - `SetShadowMapDetailDoodad` (:540-565).
  - `PostUpdate` builds the 12 matrix rows (:804-859).
  - `GetPublished` and `GetPublishedUnit` export the maps to vfog and the mask (:1262-1305).

### 1.3 Casters (WoW/Source/WorldClient/ShadowQuery.cpp)
- **Query flags:**
  - 0x1 = units
  - 0x2 = interior units below the cutoff
  - 0x4 = billboard-bone doodads (and "0x400" WMO defs)
  - 0x8 = everything else, including WMOs and terrain
- **Entities** (`GatherShadowCasters`, :950-1040):
  - Minimum radius per band is 0.25/2/10 yd. In band 0 the maximum is 25 yd when flags&0xC == 4 (:953-963).
  - Skipped: staticFlags 0x4, entity flags 0x4, and indoor entities above the cutoff (:999, :1010).
- **WMO defs** — `QueryMapObjDefsBand` handles q≤4 and the unit maps (:1131-1230); `QueryMapObjDefs` handles the q5 bands (:1234-1301):
  - Groups need flags 0x48 (exterior) (:1185, :1258), so **interior-only groups never cast**.
  - Defs with flag 0x20 are skipped. A band holds at most 0x800 groups (:1200).
  - Def flag 0x400 casts only when the band has flag 0x4, with no limit on doodad size (:1148-1156). So it **never casts into q3-4 bands**.
  - Naming is disputed: the code calls 0x400 "interior", but Types.h:92 names it MAPOBJ_FLAG_NO_DEPTH_SORT.
- **Doodads**, both WMO-linked and chunk-linked (:1208-1227, :1339-1358, :1397-1425):
  - They need m_flags 0x80 and must be within fade distance (`IsBeyondFadeDistance`, :1088).
  - Only a maximum-size filter applies, no minimum.
  - Billboard-bone models need flag 0x4.
  - ADT areas still loading asynchronously are skipped (:737, :1321).
- **Terrain** (ours, shadowTerrainCast):
  - `TerrainCastWanted` (:633-635) needs the CVar **and band flag 0x8**. So terrain casts into q3-5 bands and the q5 unit map, never at q≤2 and never into the q3-4 unit map.
  - `RenderTerrainShadow` (:637-816) draws only triangles facing away from the sun, by face and MCNR test (:708-731).
  - Those triangles are pushed 2 texels along the light, plus 1 yd in band 2 (:642-654).
  - Cached per chunk until the sun turns about 1° (:645). Holes are masked out (:762-768).
- **Far trees**: `FarTrees::DrawShadow` runs when the band has 0x8 and farTreesShadow is on (:825, :934-936).
- **M2 batches** (`CM2Model::GatherModelBatchesShadowMap`, Engine/Source/Model2/M2Model.cpp:7753-7817). Skipped:
  - material flag 0x40
  - blend above ALPHA_KEY, unless material flag 0x80 is set
  - effective alpha below 0.55
  - shader 0x8000
  - batch flag 4
  - layers above 0
  - unloaded models

  Alpha-key batches are drawn with alpha ref 0.502 (M2Model.cpp:4578-4586). The colour pass uses `element alpha × 224`.
- **WMO batches** (`CMapObj::RenderMapObjGroupsShadowMap`, MapObj.cpp:3164-3237):
  - A batch whose texture isn't loaded is skipped (:3218).
  - Alpha test 0.878 applies only to blend 1; every other blend casts as opaque.

### 1.4 Receivers (stock bytecode; the shadow maths lives in the shader permutations)
- **Effect PS index** = shadowValue + 4·pcf, plus 8 for in-shader alpha test (ShaderEffect.cpp:206-213):
  - Perms 0/4/8/12 read nothing.
  - Perms 1/5/9/13 read the unit map only (s4).
  - Perms 2-3 and the rest read s4 plus s5-7.
- **M2** (`CM2Scene::ComputeElementShaders`, M2Scene.cpp:1573-1588). Shadowed only when all of these hold:
  - the material isn't unlit (flag 1)
  - the lighting has flag 0x10, which only `CMapStaticEntity::ModelLightingCallback` sets (MapLoad.cpp:18)
  - material flag 0x100 is clear
  - the element type isn't 1

  Interior lighting (flag 8, MapLoad.cpp:24-26) gives 1, meaning the unit map only. See CM2SceneRender.cpp:353; ShaderEffect.cpp:597 binds the interior or exterior map.
- **WMO:**
  - Exterior groups (0x48) use the exterior unit map plus the bands.
  - Interior groups use `SetShadowMapGenericInterior(1)`, with shadowValue `!= 0` → 1 (MapObj.cpp:1006-1025, 1043-1070, 1417-1424, 1562-1603).
  - The permutation is chosen in `SelectWorldShaders` (:593).
- **Terrain:**
  - `GetTerrainPixelShader` (Map.cpp:441-460) picks Terrain1 (none), Terrain2 (unit map) or Terrain3 (bands) from shadowValue (MapRenderChunk.cpp:2886). The constants are set at World.cpp:3840.
  - Terrain2/3[_pcf] are patched for gfxShadowFade by Tools/CustomMpq/ShaderSrc/terrain_fade.py: S = min(map, baked MCSH), lit = 0.3·S + 0.7.
- **M2/WMO stock maths.** Example: Tools/VulkanShaders/translated/pixel/00226b5f….glsl:690-826 (Combiners_Opaque:3); MapObjOpaque:3 has the same defs.
  - Unit map: 5 point taps, a radial fade (c0 -3.448/3.414), lerped to lit by the c3 cutoff plane.
  - Bands: band 0 if |q| < 1, else band 1, else band 2, with an edge fade over 0.9-0.99.
  - S = min(unit, band).
  - Then the **terminator fade**: S' = lerp(S, 1, sat((1.2-|N·L|)^4)).
  - Then lit × (0.3·S' + 0.7), from def c8 = 1.2, 0.3, 0.7.
  - Terrain3 has no terminator fade.
- **Detail doodads** use their own VS (Tools/CustomMpq/ShaderSrc/DetailDoodad.hlsl, shadow clamped to 2) and the stock PS (DetailDoodad.cpp:172, 902-905).
- **No cascade read at all:**
  - liquids: CataWater*, VFogWater* and psLiquidProcWater. VFogWater reads only the land texture (VolumetricFogWater.cpp:953-965).
  - FarLand (FarLandPS.hlsl: N·L only)
  - the WDL horizon (MapLowDetail, unlit)
  - far-tree impostors (FarTreesPS)
  - the sky
  - the shadowed SoftParticles perms, left stock (SoftParticles.cpp:55)

### 1.5 Screen-space land mask (shadowMask)
- `RenderShadowMask` is at WoW/Source/WorldClient/VolumetricFogRenderer.cpp:1423-1521. Its shader is Tools/CustomMpq/ShaderSrc/VFogShadowMask.hlsl, drawn with GxBlend_Mod. `OnSolidDone` calls it (VolumetricFog.cpp:977-981).
- **Gates:**
  - shadowMask and gfxTerrainShadow are on, and a WDL is present (TerrainSunShadow.cpp:555-567)
  - `RendererReady`, a valid view, and interiorBlend < 1 (VolumetricFog.cpp:937-975)
  - depth copy is supported
  - the light is above the horizon (TerrainSunShadow.cpp:569-574)
- **Land T** (TerrainSunShadow.cpp:26-39; VolumetricFogRenderer.cpp:48):
  - a coarse 256² WDL lit-height texture at 33 yd per texel (±4.2 km)
  - a fine 512² texture at 4.17 yd per texel (±1067 yd)
  - a 16 yd × shadowSoft penumbra and a 4 yd surface bias
  - past the window, everything counts as lit
- **Ratio** (VFogShadowMask.hlsl:96-114):
  - stock = (A + D·ndl)(1 - k(1 - C)), with k = 0.3·gfxShadowFade (:51, :1487)
  - target = min(A, (A + D·ndl)(1 - k))
  - out = lerp(target/stock, 1, T), faded by stock fog
  - C = min(unit map, band 0 if |q| < 0.85+0.1·jitter, else bands 1/2) (:41-54; vf_shadow.hlsli:95-114)
  - N·L comes from a normal rebuilt from depth, or from the G-buffer (:101-106)
- The mask only darkens and only adds land shadow. It reads the cascades just to avoid darkening twice.

## 2. CVars (default)
| CVar | Default | Where |
|---|---|---|
| extShadowQuality | **0** (0-5) | WorldParam_CVar.cpp:702 |
| hwPCF | 1 (rejected without D24X8) | :701, cb :614-626 |
| mapShadows (baked MCSH) | 1 | :667 |
| shadowLevel (MCSH/alpha mip) | 1 | :669 |
| farTreesShadow | 1 | :757 |
| shadowCull / shadowScissor / shadowInstancing | 1 / 1 / 1 | ShadowQuery.cpp:109-111 |
| gfxSunShadows / gfxShadowSnap / gfxShadowAnchor | 1 / 1 / 1 | :112-114 |
| gfxShadowSlopeBias | 1.5 | :115 |
| shadowMapSize | 4096 (x64), 0 (Wow32) | :116, :57-61 |
| shadowSoftness | 1 | :117 |
| shadowTerrainCast | 1 | :118 |
| shadowCascadeReuse | 1 | :119 |
| gfxShadowFade | 1 | DayNight.cpp:2658 |
| gfxTerrainShadow / shadowMask / shadowSoft / shadowMaskFine | 1 / 1 / 1 / 1 | TerrainSunShadow.cpp:549-552 |
| vfogShadowedSun / vfogShafts / vfogCanopy | 1 / 1 / 0.5 | VolumetricFog.cpp:751-754 |
| shadowLOD (blob unit shadow) | 1 | shadow.cpp:231 |
| waterCaustics (sun-shadow aware) | 1 | VolumetricFogCaustics.cpp:86 |

## 3. Ways a receiver ends up unshadowed
1. **Quality setting.**
   - The default, 0, means no dynamic shadows; only MCSH applies.
   - q1-2 gives the 20 yd unit map only.
   - Mode LOWEST is used for one frame while the maps are recreated (ShadowCache.cpp:223).
2. **Cascade range.**
   - Anything beyond 640 yd of the *viewer* (the camera target) is outside the cascades. Band 2 fades out from 0.9 of its radius.
   - The bands are squares in light space, so how far they reach in the direction the camera looks varies with the sun's azimuth.
   - At q3-4, band 2 recentres only after the viewer moves 32 yd, plus 25 frames.
   - Beyond the cascades only the mask's land T applies, out to ±4.2 km; past that, nothing.
3. **Caster gaps.**
   - q3-4 bands use flags 8 only, so units and billboard-bone doodads never cast into them; they reach only the 20 yd unit map.
   - "0x400" WMO defs are absent from q3-4 bands.
   - Interior-only WMO groups never cast.
   - Size limits: an entity with radius < 2 yd drops out of band 1, and < 10 yd out of band 2. At q5, units have no shadow past 40 yd. Entities with radius > 25 yd are excluded from the q3-4 unit map.
   - Doodads beyond their fade distance, or without flag 0x80, never cast. Neither do async-loading ADT areas.
   - The group cap (0x800) and batch cap (0x2000) can cut casters off.
   - WMO batches with unloaded textures are skipped.
   - M2 batches that are blended (> ALPHA_KEY) or below 0.55 alpha are skipped. Only layer 0 casts.
   - At q5 with no exterior visible, the bands are not redrawn.
4. **Permutations that never read a shadow.**
   - Unlit M2 materials, material flag 0x100, element type 1, and any M2 not lit through ModelLightingCallback (no flag 0x10).
   - Interior WMO groups and interior-lit M2s read only the unit map. Indoors there is no sun, building or doodad shadow, and units above the player's foot plane are faded by c3.
   - Terrain1 (q0).
   - Everything listed under "No cascade read at all" in §1.4.
5. **Terrain cast gated off.**
   - shadowTerrainCast 0, q≤2, and the q3-4 unit map.
   - Sun-facing triangles never cast, by design.
   - Chunks still loading don't cast.
   - The cache isn't invalidated by height edits that keep the same pointer (ShadowQuery.cpp:753).
6. **Far land, WDL and far trees.**
   - They never sample the cascades; they rely on the mask alone.
   - The horizon is unlit, so the mask's ratio means nothing there (audit L1).
   - They get no shadow with shadowMask 0, at night, indoors, or beyond 4.2 km.
7. **Alpha-tested casters.**
   - M2 casters are cut at 0.502, but the receiver draw cuts at about 0.878·alpha, so leaf shadows come out oversized.
   - WMO batches with blend ≠ 1 cast as opaque.
   - Blended foliage (M2 blend > 1 without material flag 0x80) casts nothing.
8. **The mask disagrees with the per-material shadow.**
   - (a) The mask ignores the M2/WMO terminator fade.
   - (b) It ignores terrain's min with MCSH.
   - (c) The band 0 cutover is 0.85 in the mask and 1.0 in the receivers.
   - (d) The mask uses a point or bilinear compare, against the receivers' 5-tap PCF with shadowSoftness. It also ignores the unit map's radial fade.
   - (e) The light differs. Cascades use the latched sun clamped to 12-70° (or the steep light indoors), T uses the true sun, and N·L uses m_light1. Below 12° sun elevation, land shadows are longer than cascade shadows.
   - (f) On D3D11 with hwPCF 0, `CompareUnusable` (VolumetricFogRenderer.cpp:711-713) zeroes C, so cascade shadows that fall inside a land shadow get darkened twice.
   - (g) Transparent passes and liquids draw after the mask.
   - (h) A stale comment says terrain doesn't cast into the cascades (VolumetricFogRenderer.cpp:797; vf_shadow.hlsli:27), but shadowTerrainCast is now 1.

## 4. Backends
- **D3D9 / D3D9Ex:**
  - Stock SM3 bytecode, plus the custom.MPQ patches to Terrain2/3.
  - hwPCF is used when D24X8 is available as a depth texture (CGxDeviceD3d.cpp:2511, D3d9Ex :2508).
  - Non-PCF R32F maps are clamped to stock size.
- **D3D11:**
  - SM3 is translated to SM4 offline (/root/d3d11-port/shader-xlate/sm3hlsl.py).
  - `pcf_slots` (xlate.py:190-206) maps s4-7 for combiners, mapobj and detaildoodad perms with k&4, and s5-8 for terrain*_pcf, to `SampleCmpLevelZero` (sm3hlsl.py:792-796).
  - D24X8 is always available (CGxDeviceD3d11.cpp:1041). A mismatched shadow depth is bound depth-only (:1238).
  - hwPCF 0 disables the vfog shadowed sun and the mask's cascade term.
- **Vulkan:**
  - GLSL is generated via Tools/VulkanShaders/bls (emitter.py:790).
  - D24X8 isn't offered (CGxDeviceVulkanCaps.cpp:76-97), so Vulkan always takes the R32F path: 5 point compares at the stock 1024/2048 size, ignoring shadowMapSize.
  - Shadow maps are drawn with the frame's depth buffer; check what happens with maps larger than the window.
  - vfog and the mask take 4-tap point compares (vf_shadow.hlsli header).
- `shadow_capture` (bls/lighting.py:589-618, sm3hlsl.py:63-88) only *reads* the stock shadow factor into gxShadow, for specular and metal (lighting.py:259). It doesn't shadow anything.
- Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lighting.cpp (and D3d11Lighting.cpp) is post lighting (SSAO, height fog) with no shadow input.
- OpenGL: mode 0 only.

## 5. Size of a receiver-side rewrite
- **C++ receiver plumbing:**
  - ShadowCache.cpp/.h (1305 + 145 lines; about 250 of them are receiver binding and publishing)
  - ShaderEffect.cpp (SelectShadowShader, SetLocalLighting)
  - MapObj.cpp (22 `s_shadowInterior` sites)
  - M2Scene.cpp, CM2SceneRender.cpp, MapRenderChunk.cpp, Map.cpp, World.cpp, DetailDoodad.cpp
  - CWorldSceneRender.cpp (the cutoff)
  - VolumetricFogRenderer.cpp (the mask, about 200 lines)
  - TerrainSunShadow.cpp (693 lines)
- **Shaders and tools:** VFogShadowMask.hlsl (115 lines), vf_shadow.hlsli (130), DetailDoodad.hlsl (100), terrain_fade.py (113), build_shaders.py (251), bls/lighting.py (648), bls/emitter.py (996), sm3hlsl.py (1162), xlate.py (341).
- **Total:** about 20 files and roughly 22k lines. The code that would actually change is probably 1.5-2.5k lines.
- **Data:** about 682 stock receiver pixel permutations across 40 families: 28 Combiners_*, 7 MapObj*, Terrain2/3[_pcf] and DetailDoodad (Tools/VulkanShaders/translated/index.txt).
  - Each one has the shadow maths baked in, and each exists for 3 backends (D3D9 bytecode, SM4 DXBC, SPIR-V).
- Casters (ShadowQuery.cpp at 1429 lines, plus the M2Model and MapObj shadow paths) wouldn't need to change for a receiver-only rewrite.

## 6. Indoors (step 1 analysis)
Verdict: no contained fix. Not implemented, no `shadowInterior` CVar.

**Why flipping the receivers and casters doesn't work**
- Groups the sun can reach are already on the exterior path. The test is `m_flags & 0x48`, meaning exterior 0x8 or exterior-lit 0x40 (MapObj.cpp:1006, 1414, 1579). Open courtyards and exterior-lit halls already read the bands.
- The other groups are interior-lit: `SetLighting(group, 2/3)` gives MOCV plus interior ambient, with no sun term. In those shaders the shadow is only `lit × (0.3·S + 0.7)`, so it can only darken the baked light. Sun through a window means *adding* sun light where S = 1, and no interior permutation has a sun term to add.
- What a flip alone would do (send interior groups and flag-8 M2s through `SetShadowMapGenericInterior(0)` with the full shadowValue):
  - The perms already exist, so it would be about 40 lines.
  - Casters as they are: the exterior shell would shadow the floor under it, a near-uniform 30% darkening of every interior.
  - Interior groups also casting (drop the 0x48 test in QueryMapObjDefsBand/QueryMapObjDefs, ShadowQuery.cpp:1217/1290, and the indoor-entity skip at :999/:1010): the ceilings would cover the floors under the steep indoor light, with the same result.
  - Either way there would be no sun patches, only darker rooms.
- Casting only (interior groups into the bands for exterior receivers): almost no gain, because exterior shells enclose interiors. It still costs group cap (0x800) and batches. Skipped.

**What a real fix needs** (step 2 or later, roughly 400-650 lines plus a shader on 3 backends)
1. **Light.** Indoors, the bands use the true latched sun, not the steep light (ShadowQuery.cpp:208-217, 336-348). The unit maps keep the steep light for unit drop shadows. That needs separate band and unit light matrices (PostUpdate rows, ShadowCache.cpp:804-859). About 60 lines.
2. **Casters.** Interior groups and interior doodads cast into the bands, CVar-gated. At q5 the bands are also drawn with no exterior visible (ShadowCache.cpp:1227). About 40 lines.
3. **Interior mask.** Interior WMO and M2 draws mark a stencil bit, at the `s_shadowInterior = 1` sites in MapObj.cpp and the flag-8 M2 path. This needs a stencil on all three backends; Vulkan via `HasStencil(depthFormat)`. About 80 lines.
4. **Additive sun pass.** A screen-space pass like the shadowMask (VolumetricFogRenderer.cpp:1423), but drawn GxBlend_Add over the stencil:
   - sun colour × N·L × band visibility, from depth plus the band maps
   - placed after the WMO and M2PASS_0 draws, before the mask
   - new HLSL plus D3D11 and SPIR-V builds, about 150 lines of shader and 150 of C++
5. **Tuning.** Strength against the MOCV bake, and the indoor/outdoor hysteresis.

**Risks**
- Missing casters become bright sun patches indoors, which is far more visible than a missing shadow. Causes: unloaded-texture batches (MapObj.cpp:3218), the group and batch caps, thin walls under the bias, and portals not modelled.
- Band redraw cost while indoors.
- The light switch on entering or leaving redraws every band.
- Stencil bits may already be in use by other passes (check the SSAO/vfog stencil use).
