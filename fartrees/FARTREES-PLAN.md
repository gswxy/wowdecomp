# Far trees as octahedral impostors (farTrees)

Branch `port/fartrees`. CVar `farTrees` (default 0). Code: `WoW/Source/WorldClient/FarTrees.{h,cpp}` (draw), `FarTreesBake.{h,cpp}` (worker),
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
- Memory: per model 0.7 MB (64 px views) / 2.8 MB (128 px) of DXT5 in VRAM plus the same in RAM for the latch.
- Disk: the same per model in Cache/FarTrees.

## 4. Known limits

- Tilted placements drawn upright; WMO doodad-set trees left out; glow and replaceable-texture batches left out.
- Untested in game. If the depth write misbehaves on a backend, check FarTreesPS `o.depth` against the native
  projection's depth range first. Custom.MPQ must carry the two shaders (and the Vulkan manifest their translation).
