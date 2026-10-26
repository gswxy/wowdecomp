# doodadInstancing (branch port/doodadinst)

## Phase 1 finding: stock 3.3.5a already instances static doodads

The "N identical doodads in one draw" path exists in 12340 and is on by default:

- CVar `M2BatchDoodads` (default 1) sets M2 cache flag 0x20.
- `CM2Scene` (M2Scene.cpp ~1981) makes a batch element type 2 when `CM2Model::IsBatchDoodadCompatible`
  (cache 0x20, model fFlags 0x10 = `m_shared->m_maxInstances > 1`, batch flag 0x10, no stock point lights)
  and the batch is not transparent (blend <= alpha key and alpha >= 0.99999). Type-2 elements are hashed by
  shared/batch/texture state (`SortDoodad`), grouped, and the group head gets `m_count`.
- `CM2SceneRender::DrawBatchDoodad` draws a group as pseudo-instances: `CM2Shared::AllocInstances` grows the
  shared VB/IB to N replicated copies (bone indices offset by `instance * section.boneCount`), each copy's bones go to
  c31.. and one `Draw` covers `instances * indexCount` indices. No device-side instancing is involved, so it already
  runs unchanged on D3D9, D3D11 and Vulkan.
- Shadow maps do the same through `CModelBatchList::Optimize` + `RenderBatchShadowMap` (`shadowInstancing`, def 1).

What limits it: copies per draw = `min(0x10000 / skin indexCount, skin.boneCountMax / section.boneCount, 75 / boneCount)`.
Survey of the stock client's world `*00.skin` (common-2, expansion, lichking; 10.6k skins): `boneCountMax` is 21 in
~95% (others 53/256/64). So a one-bone bush/rock/fence draws at most 21 copies; 2700 of the 4000 common-2 skins
could take 72-75 copies by the index limit. Converted retail skins (e.g. openazeroth) carry boneCountMax 0, which
makes maxInstances 1 and they never group at all.

Blockers checked (all stock behaviour, unchanged):
- Per-instance lighting: the group uses the head element's SetupLighting; models with stock point lights never group.
- Per-instance colour/alpha: not applied per instance (stock); fading doodads (alpha < 1) are transparent → type 0.
- Blended batches: never type 2, so sort order of passes 1/2 is untouched. Groups draw after the rest of pass 0.
- Custom passes: shadows reuse the same budget (benefit too); G-buffer normals come from the same shader output;
  vfog late batches are transparent only; occlusion cull acts per model before elements are built.

Static estimate: draws for a group of N go from ceil(N/21) to ceil(N/75). 1000 visible groupable one-bone doodads:
>= 48 draws stock, >= 14 with the change, so ~30-35 draws a frame in dense forests; plus converted models that now
group at all. Small (well under 0.1 ms on D3D11/Vulkan); the big N → N/21 win is already stock.

True hardware instancing (one VB copy, per-instance matrix stream) would lift the 64K-index cap too, but needs an
instanced draw in CGxDevice (none exists), new VS permutations on both backends and a custom.MPQ rebuild; not worth
it for the remaining few draws.

## Phase 2: what is implemented

- `doodadInstancing` (default 0, WorldParam_CVar.cpp): when on and the device is D3D11 or Vulkan,
  `CM2Shared::InitializeSkinProfile` uses a 75-bone budget (the c31..c255 register file, already the clamp in both draw
  paths) instead of the skin's boneCountMax. Read when a skin loads: toggling it affects models loaded afterwards
  (relog / change map to apply to everything). D3D9 keeps the stock path. A section with boneCount 0 no longer divides
  by zero.
- `doodadInstancingStats` console command: grouped doodads drawn, draws taken, draws saved since the last call
  (counters `g_m2DoodadInstances` / `g_m2DoodadDraws` in DrawBatchDoodad). Counts stock grouping too, so compare
  the number with the CVar 0 and 1.
- No shader or device change: custom.MPQ needs no rebuild.

Memory: replicated buffers stay under the 64K-index cap per shared model (the same worst case stock allows a
boneCountMax 64/256 skin), so at most ~3 MB extra vertex data per heavily repeated model.
