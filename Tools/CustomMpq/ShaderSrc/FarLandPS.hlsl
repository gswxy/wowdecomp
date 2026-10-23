// farLand (WorldClient/FarLand.cpp): baked ground colour lit by the baked normal (DXT5nm: x in alpha, y in green)
// (FarLandBake.cpp, baked on farLand's worker) with the terrain's sun (CM2Lighting::GetSunLight, as CMapRenderChunk), then the world's fog curve. Under vfog
// the stock fog is pushed out of range, so f = 0 here and vfog's composite fogs the horizon band.
sampler2D sA : register(s0);
sampler2D sN : register(s1);
float4 gSun  : register(c0);   // world direction towards the sun
float4 gAmb  : register(c1);
float4 gDif  : register(c2);
float4 gFogC : register(c3);
float4 gFog  : register(c4);   // fog start, 1 / (end - start)
float4 gFade : register(c5);   // x: 0 -> 1 as the tile's bake arrives (0 = the stock tile: fog colour)
float4 main(float4 uv : TEXCOORD0, float dist : TEXCOORD1) : COLOR
{
    float3 a = tex2D(sA, uv.xy).rgb;
    float2 xy = tex2D(sN, uv.zw).ag * 2.0 - 1.0;
    float3 n = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    float3 lit = a * saturate(gAmb.rgb + gDif.rgb * saturate(dot(n, gSun.xyz)));
    float f = saturate((dist - gFog.x) * gFog.y);
    return float4(lerp(gFogC.rgb, lerp(lit, gFogC.rgb, f), gFade.x), 1.0);
}
