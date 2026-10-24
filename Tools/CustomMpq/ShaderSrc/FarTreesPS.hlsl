// FarTreesPS (ps_3_0): see FarTreesVS.hlsl. Mixes the 4 nearest baked views (albedo + coverage in s0; model-space
// normal + depth in s1), cuts out at coverage 0.5, lights with the sun like a doodad (ambient + diffuse x N.L),
// fogs linearly like TreeLeaves, and writes the baked depth so the quad sits in the scene as the tree's own surface.
sampler2D sAlb : register(s0);
sampler2D sNrm : register(s1);
float4 cAmb    : register(c0);
float4 cDif    : register(c1);
float4 cFogCol : register(c2);
float4 cFade   : register(c3);   // x: 0..1 while a model's atlas dissolves in after its bake
// shadowFarReceivers (FarLand::BindSunShadow): the sun's cascades, c56-c62 + c64, s5/s6, over camera-relative world.
static const float4x4 cViewToWorld = (float4x4)0;   // vf_shadow's land tap, unused here
#include "vf_shadow.hlsli"

struct PsOut {
    float4 col : COLOR0;
    float depth : DEPTH;
};

PsOut main(float4 uv01 : TEXCOORD0, float4 uv23 : TEXCOORD1, float4 w : TEXCOORD2, float4 cz : TEXCOORD3,
           float4 rot : TEXCOORD4, float4 sun : TEXCOORD5, float4 rel : TEXCOORD6,
           float4 relD : TEXCOORD7)
{
    float4 a0 = tex2D(sAlb, uv01.xy), a1 = tex2D(sAlb, uv01.zw), a2 = tex2D(sAlb, uv23.xy), a3 = tex2D(sAlb, uv23.zw);
    float4 n0 = tex2D(sNrm, uv01.xy), n1 = tex2D(sNrm, uv01.zw), n2 = tex2D(sNrm, uv23.xy), n3 = tex2D(sNrm, uv23.zw);
    float4 wc = w * float4(a0.a, a1.a, a2.a, a3.a);   // weights by coverage: an empty view adds no colour
    float cover = wc.x + wc.y + wc.z + wc.w;
    float2 cell = floor(uv01.xy * 512.0);   // dissolve cells fixed to the picture, not the screen
    float h = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.547);
    clip(min(cover - 0.5, cFade.x - h));
    wc /= cover;
    float3 alb = a0.rgb * wc.x + a1.rgb * wc.y + a2.rgb * wc.z + a3.rgb * wc.w;
    float4 nd  = n0 * wc.x + n1 * wc.y + n2 * wc.z + n3 * wc.w;

    float3 n = normalize(nd.rgb * 2.0 - 1.0);
    float ndl = dot(n, sun.xyz);
    float3 c = alb * min(cAmb.rgb + cDif.rgb * saturate(ndl), 1.0);
    c *= ReceiverShadowScale(CascadeSunVisibility(rel.xyz + relD.xyz * (nd.a * 2.0 - 1.0), 0.5), ndl);

    PsOut o;
    o.col = float4(lerp(c, cFogCol.rgb, rot.z), 1.0);
    float d = nd.a * 2.0 - 1.0 - rot.w;
    o.depth = saturate((cz.x + cz.z * d) / (cz.y + cz.w * d));
    return o;
}
