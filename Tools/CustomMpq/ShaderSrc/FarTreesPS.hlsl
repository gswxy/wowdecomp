// FarTreesPS (ps_3_0): see FarTreesVS.hlsl; a port of wxl-farfield FarFoliage.cpp kPsTree. The nearer of the two
// views (crossed over only near the 45 deg line), cut out at coverage 0.5, lit per pixel from the baked view normal
// like a doodad (min(ambient + diffuse x N.L, 1); unlit M2 surfaces at their texture colour), fogged linearly.
// The hand-over and a new picture's arrival come in by a dissolve fixed to the picture (no screen-space dither).
sampler2D sAlb : register(s0);   // rgb + coverage, 4 views side by side (256 x 64), levels 0..4 keep their coverage
sampler2D sNrm : register(s1);   // the view's normal x (right), y (up), z toward the viewer implied; b = unlit mask
float4 cAmb    : register(c0);
float4 cDif    : register(c1);
float4 cFogCol : register(c2);
float4 cFade   : register(c3);   // x: 0..1 while a model's picture dissolves in after its bake, y: coarsest level (4),
                                 // z: level bias (beyond farclip: a coarser, steadier level), w: 90 / view_blend
float4 cSun    : register(c4);   // xyz toward the sun (world)
// shadowFarReceivers (FarLand::BindSunShadow): the sun's cascades, c56-c62 + c64, s5/s6, over camera-relative world.
static const float4x4 cViewToWorld = (float4x4)0;   // vf_shadow's land tap, unused here
#include "vf_shadow.hlsli"

float4 main(float4 uv : TEXCOORD0, float4 info : TEXCOORD1, float4 right : TEXCOORD2, float4 up : TEXCOORD3,
            float4 toCam : TEXCOORD4, float4 rel : TEXCOORD5) : COLOR0
{
    // one level for both views from the first one's footprint, at most level 4 (wxl's chain), plus the bias
    float2 dx = ddx(uv.xy) * float2(256.0, 64.0), dy = ddy(uv.xy) * float2(256.0, 64.0);
    float lod = min(max(0.5 * log2(max(dot(dx, dx), dot(dy, dy))), 0.0) + cFade.z, cFade.y);
    float second = saturate((info.x - 0.5) * cFade.w + 0.5);
    float4 a = tex2Dlod(sAlb, float4(uv.xy, 0, lod)), n = tex2Dlod(sNrm, float4(uv.xy, 0, lod));
    if (second > 0.0) {
        float4 a1 = tex2Dlod(sAlb, float4(uv.zw, 0, lod)), n1 = tex2Dlod(sNrm, float4(uv.zw, 0, lod));
        a = lerp(a, a1, second);
        n = lerp(n, n1, second);
    }
    float2 cell = floor(uv.xy * float2(256.0, 64.0));   // dissolve cells fixed to the picture, not the screen
    float h = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.547);
    clip(min(a.a - 0.5, min(cFade.x, info.y) - h));

    float2 nxy = n.rg * 2.0 - 1.0;
    float3 nt  = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
    float3 nw  = normalize(right.xyz * nt.x + up.xyz * nt.y + toCam.xyz * nt.z);
    float ndl = dot(nw, cSun.xyz);
    float3 c = a.rgb * min(cAmb.rgb + cDif.rgb * saturate(ndl), 1.0);
    c *= ReceiverShadowScale(CascadeSunVisibility(rel.xyz, 0.5), ndl);
    c = lerp(c, a.rgb, n.b);   // unlit: the picture's own colour
    return float4(lerp(c, cFogCol.rgb, info.z), 1.0);
}
