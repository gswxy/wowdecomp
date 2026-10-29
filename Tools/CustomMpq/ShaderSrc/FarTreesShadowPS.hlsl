// FarTreesShadowPS (ps_3_0): see FarTreesShadowVS.hlsl. Cut out at the baked coverage (s0 alpha, 0.5 as the stock
// textured shadow batches' alpha ref; levels past 4 hold no view of their own, so none coarser is read) and write
// what the stock ShadowMapSL PS writes: max(scale * light view z, 0).
sampler2D sAlb : register(s0);
float4 cScale  : register(c0);   // w: CShadowCache::s_shadowScale (the stock PS's c0.w)

float4 main(float2 uv : TEXCOORD0, float4 z : TEXCOORD1) : COLOR0
{
    float2 dx = ddx(uv) * float2(256.0, 64.0), dy = ddy(uv) * float2(256.0, 64.0);
    float lod = clamp(0.5 * log2(max(dot(dx, dx), dot(dy, dy))), 0.0, 4.0);
    clip(tex2Dlod(sAlb, float4(uv, 0, lod)).a - 0.5);
    float d = max(cScale.w * z.z, 0.0);
    return float4(d, d, 0.0, 1.0);
}
