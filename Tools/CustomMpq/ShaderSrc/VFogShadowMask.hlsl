// VFogShadowMask (ps_3_0, full res over the world viewport; Legion survey 2.7, shadowMask): the land's sun shadow
// (gfxTerrainShadow's WDL lit-height texture, vf_shadow.hlsli) resolved once from the scene depth after the opaque
// world and laid over it as a multiply (GxBlend_Mod), so terrain, WMO and M2 receivers all darken under a mountain.
// Our own shader; Legion's shadowmask ps (91 instructions, 12 taps) was read for its role only.
//
// The receivers already shaded with the engine's shadow maps (C = min(unit map, band 0/1/2), as they do) the stock
// way: lit colour x (1 - k (1 - C)), k = 0.3 (vfog38, shadow audit H1/F1). In the land's shadow (T = 0) the surface
// should get the ambient alone (or a full cascade shadow, if darker), so the output is lerp(target / stock, 1, T) per channel, stock = (A + D ndl)(1 - k (1 - C)):
// a tree's shadow inside a mountain's merges into it (A either way) instead of staying at 0.7 of lit. Never above 1.
// ndl from a normal rebuilt from the depth (the smaller-step neighbour per axis, so silhouettes keep their own side),
// against the receivers' own light (m_light1 after PickLightBody; the land texture takes the cascades' latched light
// under gfxSunShadows, TerrainSunShadow::Update). shadowDebug (c76.w) draws a view instead, alpha blended.
// TEXCOORD0 = world-viewport uv. s0 scene depth, s5/s6 bands 1/2, s7 the land, s8 band 0, s9 the unit map, s10 the land's fine level,
// s11 its previous build while it fades in.
// c10 (shading toLight world, strength), c11 (ambient rgb, surface bias yd), c12 (diffuse rgb, k), c13 (stock fog
// start, 1 / span, on, 0), c65-c67 band 0 rows, c68-c70 unit rows, c71 (band 0 on, unit on), c72 the fine level, c73 its fade,
// c74-c75 the receivers' PCF offsets in uv (CShadowCache::s_pcfOffsets 0, 2 / 4, 6), c76 (toward the cascades' light
// world, shadowDebug), c77 (toward the land's light world, 0).
// VFogShadowMaskPoint is this source built with no hardware-compare slots, for D3D11 with R32F maps (hwPCF 0), whose
// compare slots would need the comparison sampler only depth maps get (build_shaders.py compare_slots).
#include "vf_common.hlsli"
#include "vf_shadow.hlsli"
#include "vf_gbuffer.hlsli"

float4 cMaskLight : register(c10);
float4 cMaskAmbient : register(c11);
float4 cMaskDiffuse : register(c12);
float4 cMaskFog : register(c13);
float4 cMaskRows[6] : register(c65);
float4 cMaskMaps : register(c71);
// shadowMaskFine (TerrainSunShadow::Fine): the nested ADT level of the lit heights (4.2 yd texels, +-1067 yd),
// (u0, v0, k, 1 / blend uv) as c63; it fades into s7's over its outer 1 / w of uv. w = 0: off.
float4 cTerrainFine : register(c72);
sampler2D sTerrainFine : register(s10);
// The fine level's previous build while a rebuild fades in (shadow audit P2): (u0, v0, k, w), lerp(s11, s10, w) inside
// its own window; w = 1: no fade.
float4 cTerrainFineFrom : register(c73);
sampler2D sTerrainFineFrom : register(s11);

float4 cMaskPcf[2] : register(c74);
float4 cMaskCascadeLight : register(c76);
float4 cMaskLandLight : register(c77);

sampler2D sDepth : register(s0);
sampler2D sBand0 : register(s8);
sampler2D sUnit : register(s9);

float3 MaskCoord(int r, float3 p)
{
    return float3(dot(p, cMaskRows[r].xyz), dot(p, cMaskRows[r + 1].xyz), dot(p, cMaskRows[r + 2].xyz)) +
           float3(cMaskRows[r].w, cMaskRows[r + 1].w, cMaskRows[r + 2].w);
}

// One receiver tap: the hardware's bilinear compare under hwPCF, else a point compare of the R32F map (lit when the
// stored depth is at or past the reference, as the stock perms' `cmp`).
float MapTap(sampler2D maps, float2 uv, float ref)
{
    [branch] if (cShadowControl.y > 0)
        return tex2Dlod(maps, float4(uv, ref, 0)).x;
    return tex2Dlod(maps, float4(uv, 0, 0)).x >= ref ? 1 : 0;
}

// The receivers' 5 taps (centre + c5/c7/c9/c11 of the stock perms, shadowSoftness-scaled), so the mask's idea of a
// cascade edge is as wide as the one on screen.
float ReceiverCompare(sampler2D maps, float3 q)
{
    float2 uv = q.xy * 0.5 + 0.5;
    return 0.2 * (MapTap(maps, uv, q.z) + MapTap(maps, uv + cMaskPcf[0].xy, q.z) + MapTap(maps, uv + cMaskPcf[0].zw, q.z) +
                  MapTap(maps, uv + cMaskPcf[1].xy, q.z) + MapTap(maps, uv + cMaskPcf[1].zw, q.z));
}

// What the receivers' shadow maps say, with their cutovers (Combiners_Opaque:3): the unit map inside 0.99 of its
// square, min band 0 if |q| < 1, else band 1 if |q| < 1, else band 2 faded out over 0.9-0.99. which = the map read for
// shadowDebug: 0 unit map, 1-3 bands 0-2, 4 none.
float ReceiverShadow(float3 p, out float which)
{
    float c = 1;
    which = 4;
    [branch] if (cMaskMaps.y > 0)
    {
        float3 q = MaskCoord(3, p);
        [branch] if (max(abs(q.x), abs(q.y)) < 0.99)
        {
            c = ReceiverCompare(sUnit, q);
            which = 0;
        }
    }
    float b = 1;
    float band = 4;
    float3 q0 = MaskCoord(0, p);
    [branch] if (cMaskMaps.x > 0 && max(abs(q0.x), abs(q0.y)) < 1)
    {
        b = ReceiverCompare(sBand0, q0);
        band = 1;
    }
    else if (ShadowedSun())
    {
        float3 q = ShadowCoord(0, p);
        [branch] if (max(abs(q.x), abs(q.y)) < 1)
        {
            b = ReceiverCompare(sShadowNear, q);
            band = 2;
        }
        else
        {
            q = ShadowCoord(1, p);
            float fade = saturate(11 - 11.1111 * max(abs(q.x), abs(q.y)));
            [branch] if (fade > 0)
            {
                b = lerp(1, ReceiverCompare(sShadowFar, q), fade);
                band = 3;
            }
        }
    }
    which = min(which, band);
    return min(c, b);
}

// The M2/WMO receivers' terminator fade: the shadow lifts as |N.L| drops (to 1 at 0.2), so back faces read lit.
float TerminatorFade(float3 n, float3 toLight)
{
    float t = 1.2 - abs(dot(n, toLight));
    t *= t;
    return saturate(t * t);
}

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * LinearDepth(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

// shadowDebug, drawn with alpha blending over the scene: 1 the map read, at half strength; 2 the sun's visibility;
// 3 magenta where no cascade is read (outside them all, or no world shader wrote the G-buffer: water, fixed function).
float4 DebugView(float mode, float which, float visibility, float2 pixel)
{
    [branch] if (mode < 1.5)
    {
        float3 tint = which < 0.5 ? float3(1, 1, 0) : which < 1.5 ? float3(0, 1, 0) : which < 2.5 ? float3(0, 0.4, 1)
                    : which < 3.5 ? float3(1, 0.5, 0) : float3(1, 0, 0);
        return float4(tint, 0.5);
    }
    [branch] if (mode < 2.5)
        return float4(visibility.xxx, 1);
    bool unwritten = cGBuffer.x > 0 && tex2Dlod(sNormal, float4(pixel * DepthTexelSize(), 0, 0)).a <= 0.5;
    return which > 3.5 || unwritten ? float4(1, 0, 1, 1) : 0;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float debug = cMaskCascadeLight.w;
    float2 pixel = FullPixelFromUv(viewportUv);
    float depth = SampleDepth(sDepth, pixel);
    [branch] if (IsSky(depth))
        return debug > 0 ? 0 : 1;
    float3 p = ViewRayAtUnitDepth(pixel) * LinearDepth(depth);
    float land = 1;
    [branch] if (cTerrainShadow.w > 0)
    {
        float3 world = mul(float4(p, 1), cViewToWorld).xyz;
        float lit = dot(tex2Dlod(sTerrainShadow, float4(cTerrainShadow.xy - cTerrainShadow.z * world.yx, 0, 0)).xy, 1);   // R + G: fp16 + residual
        [branch] if (cTerrainFine.w > 0)
        {
            float2 uv = cTerrainFine.xy - cTerrainFine.z * world.yx;
            float2 edge = abs(uv - 0.5);
            float w = saturate((0.5 - max(edge.x, edge.y)) * cTerrainFine.w);
            [branch] if (w > 0)
            {
                float fine = dot(tex2Dlod(sTerrainFine, float4(uv, 0, 0)).xy, 1);
                float2 from = cTerrainFineFrom.xy - cTerrainFineFrom.z * world.yx;
                [branch] if (cTerrainFineFrom.w < 1 && max(abs(from.x - 0.5), abs(from.y - 0.5)) < 0.5)
                    fine = lerp(dot(tex2Dlod(sTerrainFineFrom, float4(from, 0, 0)).xy, 1), fine, cTerrainFineFrom.w);
                lit = lerp(lit, fine, w);
            }
        }
        land = saturate(1 - (lit - world.z - cMaskAmbient.w) * cTerrainShadow.w);
    }
    [branch] if (land >= 1 && debug <= 0)
        return 1;

    float3 dx = Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0)));
    float3 dy = Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)));
    float3 n = cross(dx, dy);
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    n = normalize(ViewToWorldDirection(GBufferNormal(pixel, p, normalize(n))));
    float ndl = saturate(dot(n, cMaskLight.xyz));
    // The cascade term lifts at the terminator as the receivers' does. The land term does not: lifting it too (step 1)
    // erased a mountain's shadow on every surface under 0.2 N.L, all flat ground once the sun is under 11.5 deg, which
    // is when a mountain shades a valley, and left a tree's cascade shadow on that floor alone (2026-10-24). A back face
    // to the land's light gets no sun inside the land's shadow or out of it, so there the receivers' own result stands
    // (land 1); at N.L 0 both sides give a ratio of 1, so the cut does not show.
    float liftCascade = TerminatorFade(n, cMaskCascadeLight.xyz);
    land = dot(n, cMaskLandLight.xyz) <= 0 ? 1 : land;

    float which;
    float cascade = lerp(ReceiverShadow(p, which), 1, liftCascade);
    [branch] if (debug > 0)
        return DebugView(debug, which, min(cascade, land), pixel);
    // Terrain's own min with its baked MCSH is not modelled: MCSH is per chunk, baked for another sun, and the depth
    // does not say which pixels are terrain, so MCSH-dark ground in the land's shadow still darkens twice.
    float3 lit = cMaskAmbient.rgb + cMaskDiffuse.rgb * ndl;
    float3 stock = lit * (1 - cMaskDiffuse.w * (1 - cascade));
    // Shadows combine as min: in the land's shadow the target is the darker of the ambient and a full cascade shadow
    // (as lifted at the terminator), so a model's cascade shadow inside the land's shadow (stock < A, which this
    // multiply cannot lift) lands on the same colour as around it instead of showing through darker.
    float3 target = min(cMaskAmbient.rgb, lit * (1 - cMaskDiffuse.w * (1 - liftCascade)));
    float3 ratio = lerp(saturate(target / max(stock, 1.0 / 255)), 1, land);
    // The stock fog is in the colour when the volumetric fog did not push it out: fogged pixels keep their colour.
    float fog = saturate((length(p) - cMaskFog.x) * cMaskFog.y) * cMaskFog.z;
    return float4(lerp(1, saturate(ratio), cMaskLight.w * (1 - fog)), 1);
}
