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

## Design (this branch, revised 2026-10-23: bake at run time, cached)

Baked files shipped in an MPQ go stale on servers with custom or updated maps, so each tile is baked on the
client from the ADT it really loads, once, and cached.

- **Worker thread** (`FarLand.cpp` WorkerMain, one `std::thread`): a tile is queued the first time
  `RenderLowDetail` draws it; the worker takes the queued tile nearest the camera, reads the ADT through SFile (so
  the server's patch MPQs win), CRC32s it, and either loads `Cache\FarLand\<map>\<col>_<row>.flt` (header
  `FLT1`, version, ADT size, ADT CRC32, then the tile) when size and CRC match, or bakes (`FarLandBake.cpp`) and
  rewrites it. Layer texture means are cached per map on the worker.
- **The bake** (FarLandBake.cpp, no engine headers): albedo 128 px a tile (4.17 yd, one MCLY repeat: each layer
  texture's 4 x 4 mip mean, blended by MCAL sized by sizeAlpha; big alpha weighted sum, else layered lerp; x MCCV;
  MH2O liquids flat-coloured by LiquidType type), DXT1 levels 128..4; normals 64 px (8.3 yd, 4x the WDL's
  resolution) from MCVT, DXT5nm levels 64..4. 16.4 KB a tile.
- **Main thread**: `BeginPass` copies at most `farLandUploads` (default 4) finished tiles a frame into two window
  atlases (tile in slot row mod 32, col mod 32: albedo 4096 DXT1, normals 2048 DXT5, CPU copies latched by the
  texture callback) and uploads the slot's rectangle; atlas levels where a block spans slots get the slots' mean
  colour. It never waits on the worker (a mutex held for a queue push/pop only).
- **No popping**: an unbaked tile draws stock (fog colour); a baked one cross-fades from the fog colour over
  600 ms. An evicted slot's old tile goes back to stock and is re-queued when seen (a cache hit then).
- **Stock mesh, stock draw count, no fog changes**: as before; the shaders, one slot constant and a fade
  constant per tile are all that is added to the stock WDL draw.
- Not done (add when asked): a seed cache for stock maps (the offline tool was dropped; the first visit bakes),
  texture changes without an ADT change are not detected (the key is the ADT only), finer mesh than the WDL's
  33 yd, MCSH shadows, far water reflections.

## Measured bake cost (this server, `Tools/FarLand/bake_harness.cpp`, native clang -O2, one core)

| Map | ADT read + CRC | Bake | Worst bake |
|---|---|---|---|
| Azeroth (200 tiles from Elwynn) | 9.4 ms | 2.0 ms | 13 ms |
| Northrend (200 tiles) | 15.6 ms | 4.9 ms | 24 ms |

The worst cases are first uses of many layer textures (each read once a map). All of it is on the worker; a
cache hit costs the ADT read + CRC only.

## Cost

GPU: two texture samples and a dot product per far pixel; the same draws as stock. Memory: 16 MB of atlases on
the GPU plus the same in CPU copies, once farLand has been on. Disk: about 16 KB a tile (~11-18 MB a continent).
