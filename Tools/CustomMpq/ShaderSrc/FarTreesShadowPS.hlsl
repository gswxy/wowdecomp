// FarTreesShadowPS (ps_3_0): see FarTreesShadowVS.hlsl. Cut out at the baked coverage (s0 alpha, 0.5 as the stock
// textured shadow batches' alpha ref) and write what the stock ShadowMapSL PS writes: max(scale * light view z, 0).
sampler2D sAlb : register(s0);
float4 cScale  : register(c0);   // w: CShadowCache::s_shadowScale (the stock PS's c0.w)

float4 main(float2 uv : TEXCOORD0, float4 z : TEXCOORD1) : COLOR0
{
    clip(tex2D(sAlb, uv).a - 0.5);
    float d = max(cScale.w * z.z, 0.0);
    return float4(d, d, 0.0, 1.0);
}
