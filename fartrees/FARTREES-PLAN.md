# Far trees (farTrees): wxl-pack billboards since 2026-10-26 (section 9; sections 1-8 are the octahedral history)

Branch `port/fartrees`. CVar `farTrees` (default 1). Code: `WoW/Source/WorldClient/FarTrees.{h,cpp}` (draw), `FarTreesBake.{h,cpp}` (worker),
`Tools/CustomMpq/ShaderSrc/FarTrees{VS,PS}.hlsl` (built .bls committed).

## 1. Why the old cards failed (e6582018, 4d4185aa; removed in 1ad26f50)

Read from those diffs and `docs/archive/FARFIELD_FOG.md` "Far foliage", plus the wxl-farfield evidence in
`/root/wxl-fartrees-wt/wxl-farfield/docs/far-trees-quality-plan.md` §2.

| # | Cause | Where it came from |
|---|---|---|
| 1 | **Baked at run time on the main thread.** Up to 3 ms a frame (`kBakeMs`) rasterising skin-00 on the CPU, plus the ADT scan riding on the far-ground bake (`MapLowDetail` PumpBake), plus 33.6 MB of atlas kept as a CPU latch copy. Hitches whenever new models came in: "cost fps". | `PumpPictures`, `BakeView`, `FoliageScanTile` |
| 2 | **Hung on the whole far-field stack.** Cards needed `wdlTextured` + `horizonFog` and the stretched fog; they drew mostly past farclip inside `RenderHorizon` on the squeezed horizon depth band. With stock fog they are invisible; with the stack, the stack's own cost and look came with them. | `FarField::TerrainConstants`, `RenderFoliage` |
| 3 | **Upright cards with 4, then 8 yaw views and nothing from above.** Mixing two views 45-90 deg apart ghosts (outline IoU 0.46-0.54 on asymmetric trees); from a height a tree is a sliver; 64 px for everything. "Looked bad". | `FarFoliageVs.hlsl` `Turns`, plan §2 #1-#4 |
| 4 | **Faked depth.** Card pushed half its side behind the tree to hide under it, so terrain cut its lower half ("half trees", foliage2); lighting from a half-mesh / half-dome normal and global tint/shadow fudge factors. | `FarFoliageVs.hlsl` `back`, `kTintStrength` |
| 5 | **Flicker** from cards rebuilt facing the camera every quarter block (snap of turn + view) and a per-mip noise dissolve; plus the Vulkan occlusion-query bug that was blamed on them. | foliage2/3 notes |

## 2. Design as built (revision 2: everything at run time, 2026-10-23)

Pre-made placement and atlas files go stale on servers with custom or updated maps, so nothing is shipped.

1. **Placement at run time, off the main thread.** One worker thread (`FarTreesBake`) reads each ADT within
   farclip through SFile (so server patch MPQs win), including tiles the near world has not loaded, and returns the
   MDDF placements whose model name is a tree or bush. Main thread: at most 2 tile lists a frame become static vertex
   buffers (6 vertices a tree); tiles 600 yd past farclip are freed. Duplicate uniqueIds across ADTs are dropped.
2. **Atlas bake on the worker, cached on disk.** For a model with no valid cache the worker reads M2 + skin 00 + its
   textures, rasterises the 8 x 8 hemi-octahedral views (4x4 or 2x2 supersampled, alpha keys 224 / 128 as the game,
   blend >= 3 left out, two-sided normals, depth toward the view), dilates empty texels, builds coverage-preserving
   mips and encodes DXT5. ~0.1-0.5 s of worker time per model; zero main-thread time. The main thread takes **one
   finished model a frame** (two `TexCreate`s of DXT5 data). Until then the model draws nothing; then it dissolves in
   over 1 s (cells fixed to the picture).
   *Not a GPU bake:* Gx has no render-target readback on all three backends (`DeviceReadPixels` reads the back
   buffer; Vulkan `ReadScene` closes the frame mid-frame), and the disk cache needs the pixels. The worker bake gives
   the same pictures with no device code, so it behaves the same on D3D9, D3D11 and Vulkan and cannot hitch the frame.
3. **Cache** `Cache/FarTrees/<fnv64 of the lower-case model path>.ftc` beside the client: 'FTC1', version, u64
   fingerprint (FNV-1a over the M2, skin and texture bytes + the cache version), centre, radius, extent, framePx,
   level count, then the albedo and normal DXT5 levels. A mismatch (changed model or texture, new bake code) re-bakes
   and overwrites (written to .tmp, then renamed). Delete the folder to force a re-bake.
4. No seed data and no offline tool (the first revision's `Tools/FarTrees/bake.py` is gone; a seed could later write
   .ftc files with the same fingerprint).
5. Draw unchanged from revision 1: one draw per (visible tile, ready model); FarTreesVS picks the 4 grid views around
   the camera's direction in the model's frame (so trees do not swivel and are seen from above), FarTreesPS mixes them
   by coverage, lights `albedo x min(ambient + diffuse x N.L, 1)` with the sun, fogs linearly, writes the baked depth.
   Hand-over: opaque from 2 yd before the real tree's fade start (`s_fadeDistMin`, size classes 2-3 only), drawn
   before the M2 passes and biased 0.08 R back until its fade ends, so the fading tree blends over its own picture.

## 3. Cost (expected; to be measured in game)

- Main thread per frame: tile request loop (a few dozen tiles), at most 2 buffer fills and 1 atlas upload, one box
  test per tile, ~30-150 draws. Worker: ~1-3 MB of ADT per tile, ~0.1-0.5 s per uncached model, then cache reads.
- GPU: 2 triangles a tree, 8 fetches + clip + depth write a pixel.
- Memory: per model 0.7 MB (64 px views) / 2.8 MB (128 px) / 10.7 MB (256 px, Wow64 only; section 8) of DXT5 in VRAM plus the same in RAM for the latch.
- Disk: the same per model in Cache/FarTrees.

## 4. Known limits

- Tilted placements drawn upright; WMO doodad-set trees left out; glow and replaceable-texture batches left out.
- Untested in game. If the depth write misbehaves on a backend, check FarTreesPS `o.depth` against the native
  projection's depth range first. Custom.MPQ must carry the two shaders (and the Vulkan manifest their translation).

## 5. Sun shadows (farTreesShadow, default 1, only with farTrees)

- **Cascades** (`FarTrees::DrawShadow`, from `CShadowQuery::RenderCallback` after the terrain casters, bands 0-2 with
  query flag 8): per (tile, model) one draw of the same static buffer with FarTreesShadowVS/PS. Each tree is a quad
  of side 2R on the atlas view nearest the light (the sun is above; the hemi grid holds it), cut out at coverage 0.5,
  writing the stock ShadowMapSL value `max(c0.w * light view z, 0)` (plus hardware depth for PCF). Hand-over: the VS
  drops the tree while it is nearer than its level's fade start (`IsBeyondFadeDistance`, what the stock shadow
  queries cull real doodads by), so exactly one of the two casts. Flat at the crown's centre (no baked depth).
- **Past the cascades** (TerrainSunShadow): every placed tree as a crown of its top height (2 cz) and radius
  (sqrt(R^2 - cz^2) / sqrt 2) in the lit-height land. A worker thread (`std::async`) rasterises them on grids centred
  on the camera (fine 4.17 yd: each crown; coarse 33 yd: a cell rises only where crowns cover half of it); the builds
  take max(land, crown) per texel. Trees within the band-2 reach less ~380 yd of drift are left to the cascades; in
  the overlap the shadow mask merges the two (never darker than ambient).
- Main thread: one snapshot of the placements per new raster (~0.2 ms at 30k trees, on a 67 yd drift or when trees
  arrive), and ~0.1 ms of max() on a fine build. New rasters wait for the next build (drift, light turn), except the
  first after a map load or turning it on. Shaders: FarTreesShadowVS / FarTreesShadowPS in custom.MPQ.

## 6. Past farclip on the far land (farTreesBeyond, default 1; farTreesReach, default 0 = auto) — 2026-10-26

Row 2 of section 1 said the old cards drew past farclip on the stretched-fog stack. FarLand now owns that (real
ground past farclip, haze to 2.5 x farclip, `FarLand::FogCap`), so the impostors ride on it instead:

- **Only with the far land drawing** (`FarLand::Drawing()`, `Enable_LowDetail`); otherwise exactly as before.
- **Placement**: `Draw` asks for ADTs out to the reach (auto: `FarLand::FogCap(farclip)`, the haze end; or
  farTreesReach yd; capped at the horizon's far plane, horizonFarclipScale x farclip), nearest tile first (the worker
  takes tiles in queue order); tiles 600 yd past the reach are freed. ~113 tiles at farclip 1277 (was ~23).
- **Two passes, split by view depth** at `s_split` = min(DNInfo farclip, farclip - 50), never nearer than
  `FarLand::HorizonNear`. `DrawTrees(false)` (CWorldSceneRender, as before) draws nearer than the split; `DrawTrees(true)`
  is called from `CWorldScene::RenderHorizon` after the far land (`FarTrees::DrawBeyond`), so it uses the horizon's
  projection, its viewport depth band (s_horizonMinZ..MaxZ) and depth buffer, and is depth-tested against the far land.
  RenderHorizon is drawn once per frame, not in the Cata water reflection, and before the sky (opaque + depth write,
  so the sky / vfog composite see it as land: feedback_sky_before_blended_far_passes).
- **Depth**: FarTreesPS `o.depth` is mapped into the viewport's depth range (PS c4.xy) since oDepth / SV_Depth /
  gl_FragDepth skip the viewport transform; the near pass gets 0..1, the beyond pass the horizon band.
- **Look**: beyond pass fog = FarLandPS's haze (ray length x mean density e^-(rise / farLandHazeHeight), capped at
  `FarLand::kFogMax` 0.85, now in FarLand.h), same DNInfo fog curve, so trees fade with the ground. Cascade shadows
  as before (BindSunShadow; past the cascades nothing), canopy bumps in TerrainSunShadow now include the far trees.
- **Shimmer / cost**: beyond farclip one mip coarser (`kBeyondLodBias`, PS now picks one tex2Dlod level from the first
  view's footprint for all 8 taps); past the split a hash of the placement keeps p = split / depth (>= 0.25) of the
  trees (no longer grown, see section 8); over the last 15 % of the reach the trees shrink to nothing (no edge line).
  kMinPixels 2 still culls.
- VS c10/c11, PS c4 are new (SavedConstants restores 12 VS / 5 PS registers).
- Untested in game. Watch: depth fighting of trunks with the far land at 2-3 km (the band is 1/1024 of the depth
  range: ~1 yd steps at 3 km), the hand-over at farclip - 50 (a tree pops between passes only if the two projections
  disagree), worker catch-up after a teleport (far tiles queue ahead of models), memory on Wow32.

## 7. Player MPQs that replace trees or ground textures — 2026-10-26

Every bake reads through the client's MPQ chain (SFile), so a player's own MPQ is what gets baked. Cache validity:
- **FarTrees** (Cache/FarTrees/*.ftc): the fingerprint already covers the .m2, its 00.skin and the raw bytes of
  every texture a batch samples (hashed in LoadMesh even with decode false, so a cache hit still decodes nothing).
  A model-only or texture-only replacement re-bakes; no change needed.
- **FarLand** (Cache/FarLand/<map>/*.flt): was keyed by ADT size + CRC32 + WDT/liquid only, but the ground colour
  is the mean of each MTEX tileset BLP, so a retexture MPQ kept the stale colours. Now the header carries texCrc
  (`FarLandBake::TexturesKey`: CRC of those means, memoised in ctx.texMean, so Bake re-reads nothing);
  kCacheVersion 3, every old .flt re-bakes once. Published as v2026.10.26-0656-texkey.

## 8. "Massive trees where there is a small tree", "resolution too low" — 2026-10-26 (after v2026.10.26-0656-texkey)

Checked against the client's own MPQs (124 tree models over Elwynn, Ashenvale, Grizzly Hills, Zangarmarsh and more,
scratch script): MDDF scale (u16 / 1024) is applied as stock (CMap::CreateDoodadDef), MMID/MMDX mapping and positions
match stock, every MDDF entry is MCRF-referenced, header box = drawn-vertex box on every model, bone scale tracks ~1.
So the bake's card (2R from the drawn skin-00 vertices) is the real tree's size. The causes were in the draw:

1. **Grown survivors past farclip** (16a1692d): FarTreesVS multiplied R by 1 / sqrt(p), up to 2x linear (4x area) at
   4 x the split. Removed: the thinning stays, nothing grows.
2. **Low mips turn a tree into a block of its card's size** (since c4e59784): the PS took any mip down to 1 x 1. At
   1-2 texels a view (a 64 px view at mip 5-6; the beyond pass's +1 bias reaches it at a few px on screen) bilinear
   spreads one coverage value over the whole 2R square, and the coverage-keeping rescale then passes either the whole
   square or nothing. A pine (2R ~ its height, ~3x its width) became a square blob. FarTreesPS now clamps the level at
   cFade.y = log2(framePx) - 2 (each view keeps 4 x 4 texels); smaller trees alias a little instead of growing.
3. **Views too small for the hand-over**: 64 px below a 40 yd box, 128 above. At 1440 p (WoW's ~0.91 rad vertical
   fov, ~1500 px per yd at 1 yd) a class-3 canopy (2R 68-130 yd) at its 730 yd fade start is 140-270 px, a class-2
   tree (2R ~13 yd) at 185 yd ~100 px. framePx is now the smallest of 64 / 128 / 256 that shows the card at most 4/3
   magnified there (kPxPerYd, BakeAtlas); 256 only on Wow64 (kMaxFramePx 128 on Wow32: address space, and the
   256 bake holds ~150 MB of float images on the worker). Cache file names carry kMaxFramePx (Wow32 and Wow64 in one
   folder keep their own), kCacheVersion 2: everything re-bakes once.

Memory (RAM latch + VRAM, alb + nrm, mips): 64 px 1.3 MB, 128 px 5.3 MB, 256 px 21 MB a model. Over the 124 surveyed
models: before 357 MB; now Wow64 873 MB (51 x 64, 47 x 128, 26 x 256), Wow32 457 MB (51 x 64, 73 x 128). Only the
models in reach are resident and kBudget (Wow64 512 MB, Wow32 128 MB) LRU-drops atlases idle > 10 s.
Untested in game. If the hand-over still pops at 4K, raise kPxPerYd (one constant, then bump kCacheVersion).

## 9. wxl-pack billboards replace the octahedral impostors — 2026-10-26 (v2026.10.26-0947-ftwxl)

User: "they draw the wrong trees and some trees are too large ... it should function similar to the billboard trees
in wxl-pack". Audits: scratchpad fartrees-audit/README.md. Ported 1:1 from /root/wxl-farbush-wt (wraithhost-lite
FarLod.cpp / Impostor.cpp bake, wxl-farfield FarFoliage.cpp draw); sections 2.2, 2.5, 4, 5 (cascade quad) and 8 (view
size) no longer describe the code.

- **Filter** (FarTreesBake.cpp IsTree / HeaderOf): the whole lower-case path has tree / palm / kalidarbush /
  felwoodbush and none of street, leaves (unless noleaves), stump, log, root (unless treeroot); not wxl: also none of
  banner, hut, bridge, dock, cloak, spine (losttreehuts, nightelftreebanner02, sholazarpalm_bridge ... are props).
  Header box height 0.5 - 500 and width > 0.1; placed header height x scale >= 6 yd (kTreeMinHeight). The old
  leaf-substring list (pine, oak, canopy, cedar, bush, shrub ...), the 4 yd floor and kMaxScale are gone.
- **Leaf keying**: the game shares one CM2Shared per leaf name (M2Cache.cpp); the worker maps each leaf to the first
  full path it sees and every placement of that name uses it.
- **Dedup**: s_seen maps uniqueId -> the tile that placed it; FreeTile only drops its own ids (border trees no longer
  vanish when a neighbour that skipped them is freed).
- **Bake** (Impostor.cpp Render): 4 side views every 90 deg about model z, 64 px from a 128 px raster (2x2), frame =
  max(zHi - zLo, 2 x max sqrt(x^2 + y^2)) and bottom = zLo over every vertex, bottom-aligned; textures at their <= 256 px
  level; batch colour x transparency at frame 0; two-texture combiners; keys 224/255 (blend 1), 0.5 (blend 2); unlit
  mask (render flag 1); view-space normal; 8 3x3 fill passes; per view 5 coverage-kept levels (64 .. 4). Atlas 256 x 64
  (views side by side), full DXT5 chain (levels 5-8 for the device only). Albedo + normal both DXT5 (wxl: DXT1 normal).
- **Card** (FarTreesVS): side = frame x scale, bottom pivot at z + bottom x scale, along the placement's lean axis
  (u = Rz(rot.y+180) Ry(rot.x) Rx(rot.z) z, GxVBF_PT2 tex1), turned about it to face the camera. View = nearest of
  the 4 (yaw = rot.y + 180), crossing over only within 12 deg of the 45 deg line; uv clamped 1/64 inside each view.
  PS: one level from the first view's footprint, clamped at 4 (+1 beyond farclip), clip 0.5, light min(amb + dif N.L, 1)
  with the normal rebuilt on the card's right / up / toward-camera axes, unlit mask, cascade receiver shadow, linear fog.
  No baked depth any more (o.depth removed: the hardware maps the card into either pass's depth band).
- **Hand-over** (FarFoliage.cpp byClass / byPlane / back): D = fade start + stretch B of the size class (2, 3 and now
  4: giants kept), the card comes in over [D - 2B, D - B]; and over tree_band 300 yd ending tree_lead 40 yd short of
  min(farclip, 2086). While dist < D + 47 + 20 the card stands 0.5 x side further along the view ray (behind its tree,
  same screen size). The fade is the existing dissolve fixed to the picture (hash of the atlas texel, min with the
  1 s arrival fade): deviation, wxl blends the fading cards in a second pass. No dither in composites.
- **Shadow casters** (FarTreesShadowVS): the same card turned to face the sun on the view nearest the sun's azimuth;
  the real tree casts until its class's fade start (classes 2-4), the card after.
- **Kept**: fingerprint cache (now kCacheVersion 3, file names without the old -<px> suffix; every old .ftc is
  ignored), the beyond-farclip pass and its thinning / shrink / haze, farTrees* CVars, canopies (top = z + (bottom +
  height) x scale, radius reach / sqrt 2).
- **Not ported**: the glow picture (blend 3/4 layers, Impostor.cpp Picture::glow), MPHD 0x8's early size-class stop,
  wxl's bush layer (.farbush), the giant meshes past farclip (.fartrm), card colour match (gMatch.xy).
- Check: `python3 -I fartrees/check_filter.py` (filter over the stock listfile, frame/bottom of sample models).
- Untested in game. Look at: Elwynn / Grizzly Hills hand-over at 170-200 yd (class 2) and 710-750 yd (class 3), leaning
  Northrend trees, giant Kalidar / Teldrassil trees (class 4), and that tree-named props no longer get cards.
