// VFogWakeSplat (ps_3_0, additive into the wide Gr1616F ripple map): wxl-water kRippleSplat as SplatHulls draws it, one
// ship's waterline polygon fanned from its centroid, every vertex at the bump's centre (perturbtex.blp at uv 0.5: 248 / 255),
// so the whole shape adds one flat height. c0.x = the splat (wxl-water clamp(L.splat * strength) * sin, times that texel).
float4 cSplat : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(cSplat.x, 0, 0, 0);
}
