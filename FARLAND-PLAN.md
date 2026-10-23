# farLand: real ground colour + baked normals on the far land

Branch `port/farland`. CVar `farLand` (0 default = stock horizon, 1 = on).

## Why the 2026-10-17 stack failed (removed in 1ad26f50)

Read from 413f7c4, 9b70f5d, 90928f1, 2a753377, a3f9818d, e5ef0b0 and docs/archive/FARFIELD_FOG.md.

1. **Runtime baking on the main thread.** Every ADT in reach was read again and composited in the client
   (heights, MCLY x MCAL, three 4096 DXT1 atlases, DXT encode), "about 2 ms a frame plus one slice", 2 detailed
   tile meshes a frame, plus farTrees' picture bake (3 ms a frame). That is the fps cost, and it never ends:
   moving the camera keeps re-baking window atlases.
2. **It changed the whole world's fog.** horizonFog stretched `DNInfo` fog end by horizonFarclipScale (4x) for
   everything, so the near world lost most of its fog, and the hard edge of the loaded ADT ring and the object
   cull at farclip became visible. The blend band (redraw of the edge chunks with an extra shader) was a patch
   for that, and more cost.
3. **Patchy and popping.** Unbaked tiles drew fog-coloured among baked ones; three atlas resolutions picked by
   distance, plus farTerrainLod levels 0/1/2 without geomorph, swapped as the camera moved.
4. **Shading from 33 yd normals.** The WDL tiles were lit with vertex normals from the WDL grid, so slopes were
   soft blobs; first version used minimap textures (wrong colour, baked lighting).
5. **Too much surface at once** (five features, ~3000 lines, new CVars each) - nothing could be judged alone.

## Design (this branch)

- **All baking offline** (`Tools/FarLand/bake_farland.py`, ~1 min a continent): per map, an albedo atlas
  (128 px a tile = 4.17 yd, one texture repeat; MCLY layer means blended by MCAL, big alpha weighted sum, x MCCV,
  liquids flat-coloured) and a normal atlas (64 px a tile = 8.3 yd, 4x finer than the WDL mesh, from MCVT heights;
  DXT5nm). Tiles packed sparsely, 32 x 32 slots (64 x 32 above 1024 tiles: Northrend). Shipped in custom.MPQ
  as `FarLand\<Map>.flnd/_a.blp/_n.blp`. Runtime CPU work: zero.
- **Stock mesh, stock draw count.** `CMap::RenderLowDetail` keeps its buffers, holes and draws; `FarLand::Apply`
  only binds a VS/PS pair (FarLandVS/PS), the two atlases and one constant (the tile's slot). No new buffers,
  no LOD levels, nothing to pop. One state: a tile is baked or it is stock.
- **No fog changes.** The pass uses `DNInfo::m_fog` as the terrain does. Under vfog (default) that fog is pushed
  out of range and vfog's composite already fogs the horizon depth band by real distance, so the far land shows
  through it. With vfog 0 the stock curve ends at farclip, so the far land comes out fog-coloured = stock.
- **Lighting** is the terrain's sun (CM2Lighting::GetSunLight, as CMapRenderChunk): ambient + diffuse x N.L
  with the baked normal, so distant slopes turn with the sun.
- **Seams**: a tile edge maps to its edge texel's centre, so mip 0 never samples the unrelated neighbour slot.
  Known corner: lower mips can bleed half a texel across slots (faint lines at very long range).
- Not done (add when asked): finer mesh than the WDL's 33 yd (silhouettes stay WDL), MCSH baked shadows,
  far water reflections, far WMOs.

## Cost

GPU: two texture samples and a dot product per far pixel; the same draws as stock. VRAM: about 16 MB a map
(Northrend 32 MB). Disk: 17 MB a map in custom.MPQ before zlib.

## Shipping steps (not done by this branch)

`python3 -I Tools/FarLand/bake_farland.py` -> `Tools/CustomMpq/FarLand`, then the usual
`translate_shaders.py` (Vulkan) + `build_custom_mpq.py`. FarLandVS/PS .bls (D3D9/D3D11) are committed.
