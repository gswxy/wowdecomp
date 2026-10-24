// farLand (WorldClient/FarLand.cpp): baked ground colour lit by the baked normal (DXT5nm: x in alpha, y in green)
// (FarLandBake.cpp, baked on farLand's worker) with the terrain's sun (CM2Lighting::GetSunLight, as CMapRenderChunk), then the world's fog curve. Under vfog
// the stock fog is pushed out of range, so f = 0 here and vfog's composite fogs the horizon band.
sampler2D sA : register(s0);
sampler2D sN : register(s1);
float4 gSun  : register(c0);   // world direction towards the sun
float4 gAmb  : register(c1);
float4 gDif  : register(c2);
float4 gFogC : register(c3);
float4 gFog  : register(c4);   // fog start, 1 / (end - start), most fog the land takes (kFogMax: a silhouette)
float4 gFade : register(c5);   // x: 0 -> 1 as the tile's bake arrives (0 = the stock tile: fog colour)
float4 gAtlas : register(c6);  // albedo slot texels, 1 / atlas texels; normal the same
// The atlas uv of tile coordinate t (0..1) in the slot at origin, at the mip trilinear reads (capped at maxLod): inset
// by half a texel of the coarser mip, so a tile edge never filters in the next slot (another or no tile). At mip 0
// that is the old edge-texel-centre mapping.
float4 Slot(float2 t, float2 origin, float px, float inv, float maxLod)
{
    float lod = clamp(log2(max(length(ddx(t) * px), length(ddy(t) * px))), 0, maxLod);
    float h = 0.5 * exp2(ceil(lod));
    return float4(origin + (h + t * (px - 2 * h)) * inv, 0, lod);
}
float4 main(float4 uv : TEXCOORD0, float dist : TEXCOORD1) : COLOR
{
    // albedo to mip 4 (8 texels a tile), normals to mip 3 (8 texels; their levels past 4 are flat)
    float3 a = tex2Dlod(sA, Slot(uv.xy, uv.zw, gAtlas.x, gAtlas.y, 4)).rgb;
    float2 xy = tex2Dlod(sN, Slot(uv.xy, uv.zw, gAtlas.z, gAtlas.w, 3)).ag * 2.0 - 1.0;
    float3 n = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    float3 lit = a * saturate(gAmb.rgb + gDif.rgb * saturate(dot(n, gSun.xyz)));
    float f = min(saturate((dist - gFog.x) * gFog.y), gFog.z);
    return float4(lerp(gFogC.rgb, lerp(lit, gFogC.rgb, f), gFade.x), 1.0);
}
