// VFogRayMask (ps_3_0, ray target = world viewport / 4): port of coa-vfog shaders/vf_ray_mask.hlsl. The bright
// sky around the sun. TEXCOORD0 = ray target uv instead of VPOS (vf_common.hlsli). s0 scene depth, s1 the
// full-res scene copy before the fog, sampled bilinear at the ray texel centre (coa StretchRect'd it into a
// quarter-res target with D3DTEXF_LINEAR first: the same taps).
#include "vf_common.hlsli"

float4 cRayMask : register(c9);
float4 cRaySource : register(c10);
// xy = the sun's pixel (world viewport px), z = the disc test radius px, w = 1 test on. 2026-10-24: with the disc behind a
// mountain, a model or the player, bright sky gaps within the wide falloff fanned out as spotlight rays; the mask now
// scales by how much of the disc's neighbourhood is open sky (pixels clamped into the viewport when it is off screen).
float4 cSunDisc : register(c11);

sampler2D sDepth : register(s0);
sampler2D sSceneBeforeFog : register(s1);

static const float3 kLumaWeights = float3(0.299, 0.587, 0.114);
static const float kThresholdRampSlope = 4;

float4 main(float2 rayUv : TEXCOORD0) : COLOR0
{
    float2 pixel = min(ViewportOrigin() + rayUv * ViewportSize(), ViewportLastPixelCentre());
    float skyMask = IsSky(SampleDepth(sDepth, pixel)) ? 1 : 0;
    float3 viewDirection = normalize(ViewRayAtUnitDepth(pixel));
    float sunFalloff = pow(saturate(dot(viewDirection, cRayMask.xyz)), cRayMask.w);
    float3 scene = tex2Dlod(sSceneBeforeFog, float4(rayUv, 0, 0)).rgb;
    float luminance = dot(scene, kLumaWeights);
    float discSky = 1;
    [branch] if (cSunDisc.w > 0)
    {
        discSky = 0;
        [unroll] for (int k = 0; k < 9; k++)
        {
            float2 offset = float2(k % 3, k / 3) - 1;
            float2 tap = clamp(cSunDisc.xy + offset * cSunDisc.z, ViewportOrigin() + 0.5, ViewportLastPixelCentre());
            discSky += IsSky(SampleDepth(sDepth, tap)) ? 1.0 / 9 : 0;
        }
    }
    float brightSky = skyMask * discSky * discSky * sunFalloff * saturate((luminance - cRaySource.y) * kThresholdRampSlope);
    return float4(scene * brightSky, 1);
}
