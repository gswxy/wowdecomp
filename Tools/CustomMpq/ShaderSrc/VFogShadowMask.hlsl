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
// shadowUnified (c71.z, docs/shadow-research/05-step2-consolidation.md): the exterior receivers drew without their
// cascade term (shadow value 0 permutations until this pass), so this pass is the sun's one visibility answer for them:
// ratio = min(cascade target, land target) / lit, both terms lifted at the terminator the M2/WMO way (terrain too
// now), one fog rule. Receivers that still shaded themselves (interior groups and models, q1-2: their pixel shaders
// declare the shadow sampler) tag the G-buffer alpha 0.25 / 0.75 (normals.py) and keep the ratio above.
// shadowPlaneBias (c71.w, shadowbias 2026-10-25): every tap of a compare takes the receiver's own depth plane,
// q.z + dot(dz/duv, offset) (Isidoro GDC 2006; NVIDIA PCSS gradient bias), from the rebuilt surface's shadow-space
// slope, capped at 2 texels of depth: a tap one texel off a grazing wall otherwise reads the wall itself as a caster.
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
float4 cMaskMaps : register(c71);   // (band 0 on, unit on, shadowUnified, shadowPlaneBias)
// shadowMaskFine (TerrainSunShadow::Fine): the nested ADT level of the lit heights (4.2 yd texels, +-1067 yd),
// (u0, v0, k, 1 / blend uv) as c63; it fades into s7's over its outer 1 / w of uv. w = 0: off.
float4 cTerrainFine : register(c72);
sampler2D sTerrainFine : register(s10);
// The fine level's previous build while a rebuild fades in (shadow audit P2): (u0, v0, k, w), lerp(s11, s10, w) inside
// its own window; w = 1: no fade.
float4 cTerrainFineFrom : register(c73);
sampler2D sTerrainFineFrom : register(s11);

float4 cMaskPcf[2] : register(c74);
// shadowPenumbra / shadowMaxThrow (TerrainSunShadow::SoftShadow, docs/shadow-research/06-soft-shadows.md), unified pixels only:
// c80 (filter radius per yard to the caster, throw clamp on, any on, shadowMaskSmooth: the fine level's texels per side or
// 0), c81 the blocker search's reference offsets (yd).
float4 cMaskSoft : register(c80);
float4 cMaskSoftLevels : register(c81);
float4 cMaskCascadeLight : register(c76);
float4 cMaskLandLight : register(c77);

sampler2D sDepth : register(s0);

// The fine level's lit height (R + G) at uv: bilinear, or under shadowMaskSmooth (c80.w = texels per side) a cubic
// B-spline from 4 bilinear taps, so the land shadow's edge curves through the 4 yd cells instead of running straight
// across each one (docs/shadow-research/07-land-edge.md).
float FineLit(sampler2D heights, float2 uv)
{
    float n = cMaskSoft.w;
    [branch] if (n <= 0)
        return dot(tex2Dlod(heights, float4(uv, 0, 0)).xy, 1);
    float2 t = uv * n - 0.5;
    float2 f = frac(t);
    float2 i = t - f;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6, w1 = (4 - 6 * f2 + 3 * f3) / 6, w3 = f3 / 6;
    float2 g0 = w0 + w1, g1 = 1 - g0;
    float2 p0 = (i - 0.5 + w1 / g0) / n, p1 = (i + 1.5 + w3 / g1) / n;
    return g0.y * (g0.x * dot(tex2Dlod(heights, float4(p0.x, p0.y, 0, 0)).xy, 1) + g1.x * dot(tex2Dlod(heights, float4(p1.x, p0.y, 0, 0)).xy, 1)) +
           g1.y * (g0.x * dot(tex2Dlod(heights, float4(p0.x, p1.y, 0, 0)).xy, 1) + g1.x * dot(tex2Dlod(heights, float4(p1.x, p1.y, 0, 0)).xy, 1));
}
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
// The receiver-plane reference of a tap at uv offset o: grad.xy = dz/du, dz/dv, grad.z = the cap (2 texels of depth).
float PlaneRef(float z, float3 grad, float2 o)
{
    return z + clamp(dot(grad.xy, o), -grad.z, grad.z);
}

float ReceiverCompare(sampler2D maps, float3 q, float3 grad)
{
    float2 uv = q.xy * 0.5 + 0.5;
    return 0.2 * (MapTap(maps, uv, q.z) + MapTap(maps, uv + cMaskPcf[0].xy, PlaneRef(q.z, grad, cMaskPcf[0].xy)) +
                  MapTap(maps, uv + cMaskPcf[0].zw, PlaneRef(q.z, grad, cMaskPcf[0].zw)) +
                  MapTap(maps, uv + cMaskPcf[1].xy, PlaneRef(q.z, grad, cMaskPcf[1].xy)) +
                  MapTap(maps, uv + cMaskPcf[1].zw, PlaneRef(q.z, grad, cMaskPcf[1].zw)));
}

// dz/du, dz/dv of the receiver's plane in a map (dqx, dqy: its q across one pixel right / down, from the rebuilt
// neighbours), and the cap: 2 texels of depth at the map's ortho scale (zyd / uvyd per uv). 0 under shadowPlaneBias 0,
// or where the pixel's footprint in the map is degenerate (the surface edge-on to the light).
float3 PlaneGradient(float3 dqx, float3 dqy, float zyd, float uvyd)
{
    float2 ux = dqx.xy * 0.5, uy = dqy.xy * 0.5;    // uv = q.xy * 0.5 + 0.5
    float det = ux.x * uy.y - uy.x * ux.y;
    float cap = 2 / cShadowControl.w / max(uvyd, 1e-6) * zyd;
    [branch] if (cMaskMaps.w <= 0 || abs(det) < 1e-14)
        return 0;
    float2 g = float2(uy.y * dqx.z - ux.y * dqy.z, ux.x * dqy.z - uy.x * dqx.z) / det;
    return float3(g, cap);
}

// PCSS from compares alone (the hwPCF maps are depth textures read through the compare, no raw depth): moving the
// reference L yd toward the light, a tap reads lit iff its caster is within L yd of the receiver along the light, so
// the shadowed fraction at L = 0, L1..L4 (c81) over 5 search taps is the share of casters further back than each L.
// Their mean distance d sets the filter radius (d x c80.x: the sun disc's penumbra), so a shadow is sharp where the
// caster touches the receiver and soft far from it; shadowMaxThrow (c80.y) drops the casters past L4 and half of those
// between L3 and L4. zyd / uvyd: the map's q.z and uv per yard (its rows are an ortho light: one length each).
// distance (out): d, -1 with no caster (shadowDebug 8); share (out): what shadowMaxThrow keeps of the shadow.
float SoftCompare(sampler2D maps, float3 q, float3 grad, float zyd, float uvyd, out float distance, out float share)
{
    float2 uv = q.xy * 0.5 + 0.5;
    float pcf = length(cMaskPcf[0].xy);
    float search = max(cMaskSoftLevels.w * cMaskSoft.x * uvyd, pcf);
    float2 taps[5] = { float2(0, 0), float2(0.7071, 0.7071) * search, float2(-0.7071, 0.7071) * search,
                       float2(0.7071, -0.7071) * search, float2(-0.7071, -0.7071) * search };
    float b0 = 0;
    [unroll] for (int i = 0; i < 5; i++)
        b0 += 1 - MapTap(maps, uv + taps[i], PlaneRef(q.z, grad, taps[i]));
    distance = -1;
    share = 1;
    [branch] if (b0 <= 0)
        return 1;
    float b[4] = { 0, 0, 0, 0 };
    [unroll] for (int k = 0; k < 4; k++)
        [unroll] for (int j = 0; j < 5; j++)
            b[k] += 1 - MapTap(maps, uv + taps[j], PlaneRef(q.z, grad, taps[j]) - cMaskSoftLevels[k] * zyd);
    // mean caster distance: each slice between offsets at its middle, the last past L4 at L4
    distance = ((b0 - b[0]) * 0.5 * cMaskSoftLevels.x + (b[0] - b[1]) * 0.5 * (cMaskSoftLevels.x + cMaskSoftLevels.y) +
                (b[1] - b[2]) * 0.5 * (cMaskSoftLevels.y + cMaskSoftLevels.z) + (b[2] - b[3]) * 0.5 * (cMaskSoftLevels.z + cMaskSoftLevels.w) +
                b[3] * cMaskSoftLevels.w) / b0;
    share = cMaskSoft.y > 0 ? saturate((b0 - 0.5 * (b[2] + b[3])) / b0) : 1;
    [branch] if (share <= 0)
        return 1;
    // The receivers' 5 taps scaled out to the radius (the stock pattern at contact), with 4 more between them at half
    // of it once it is wider than the stock taps, so a wide penumbra is a ramp rather than five copies.
    float s = max(distance * cMaskSoft.x * uvyd / max(pcf, 1e-6), 1);
    float v = 0.2 * (MapTap(maps, uv, q.z) + MapTap(maps, uv + cMaskPcf[0].xy * s, PlaneRef(q.z, grad, cMaskPcf[0].xy * s)) +
                     MapTap(maps, uv + cMaskPcf[0].zw * s, PlaneRef(q.z, grad, cMaskPcf[0].zw * s)) +
                     MapTap(maps, uv + cMaskPcf[1].xy * s, PlaneRef(q.z, grad, cMaskPcf[1].xy * s)) +
                     MapTap(maps, uv + cMaskPcf[1].zw * s, PlaneRef(q.z, grad, cMaskPcf[1].zw * s)));
    [branch] if (s > 1)
    {
        float h = 0.5 * s * 0.7071;
        float4 o0 = float4(cMaskPcf[0].x - cMaskPcf[0].y, cMaskPcf[0].x + cMaskPcf[0].y, cMaskPcf[0].z - cMaskPcf[0].w, cMaskPcf[0].z + cMaskPcf[0].w) * h;
        float4 o1 = float4(cMaskPcf[1].x - cMaskPcf[1].y, cMaskPcf[1].x + cMaskPcf[1].y, cMaskPcf[1].z - cMaskPcf[1].w, cMaskPcf[1].z + cMaskPcf[1].w) * h;
        float inner = 0.25 * (MapTap(maps, uv + o0.xy, PlaneRef(q.z, grad, o0.xy)) + MapTap(maps, uv + o0.zw, PlaneRef(q.z, grad, o0.zw)) +
                              MapTap(maps, uv + o1.xy, PlaneRef(q.z, grad, o1.xy)) + MapTap(maps, uv + o1.zw, PlaneRef(q.z, grad, o1.zw)));
        v = lerp(v, (5 * v + 4 * inner) / 9, saturate(s - 1));
    }
    return 1 - (1 - v) * share;
}

// One map's term: the receivers' own compare, or under shadowUnified with c80.z the soft one. rows: its q.x and q.z rows.
float MapShadow(sampler2D maps, float3 q, float3 dqx, float3 dqy, bool soft, float4 rowX, float4 rowZ, inout float distance, inout float share)
{
    float zyd = length(rowZ.xyz), uvyd = 0.5 * length(rowX.xyz);
    float3 grad = PlaneGradient(dqx, dqy, zyd, uvyd);
    [branch] if (!soft)
        return ReceiverCompare(maps, q, grad);
    float d, k;
    float v = SoftCompare(maps, q, grad, zyd, uvyd, d, k);
    [branch] if (d > distance)   // the caster furthest back of the maps read, for shadowDebug 8
    {
        distance = d;
        share = k;
    }
    return v;
}

// What the receivers' shadow maps say, with their cutovers (Combiners_Opaque:3): the unit map inside 0.99 of its
// square, min band 0 if |q| < 1, else band 1 if |q| < 1, else band 2 faded out over 0.9-0.99. which = the map read for
// shadowDebug: 0 unit map, 1-3 bands 0-2, 4 none.
float ReceiverShadow(float3 p, float3 dx, float3 dy, bool soft, out float which, out float unitOnly, out float bandOnly, out float distance, out float share)
{
    float c = 1;
    which = 4;
    distance = -1;
    share = 1;
    [branch] if (cMaskMaps.y > 0)
    {
        float3 q = MaskCoord(3, p);
        [branch] if (max(abs(q.x), abs(q.y)) < 0.99)
        {
            c = MapShadow(sUnit, q, MaskCoord(3, p + dx) - q, MaskCoord(3, p + dy) - q, soft, cMaskRows[3], cMaskRows[5], distance, share);
            which = 0;
        }
    }
    float b = 1;
    float band = 4;
    float3 q0 = MaskCoord(0, p);
    [branch] if (cMaskMaps.x > 0 && max(abs(q0.x), abs(q0.y)) < 1)
    {
        b = MapShadow(sBand0, q0, MaskCoord(0, p + dx) - q0, MaskCoord(0, p + dy) - q0, soft, cMaskRows[0], cMaskRows[2], distance, share);
        band = 1;
    }
    else if (ShadowedSun())
    {
        float3 q = ShadowCoord(0, p);
        [branch] if (max(abs(q.x), abs(q.y)) < 1)
        {
            b = MapShadow(sShadowNear, q, ShadowCoord(0, p + dx) - q, ShadowCoord(0, p + dy) - q, soft, cShadowBands[0], cShadowBands[2], distance, share);
            band = 2;
        }
        else
        {
            q = ShadowCoord(1, p);
            float fade = saturate(11 - 11.1111 * max(abs(q.x), abs(q.y)));
            [branch] if (fade > 0)
            {
                b = lerp(1, MapShadow(sShadowFar, q, ShadowCoord(1, p + dx) - q, ShadowCoord(1, p + dy) - q, soft, cShadowBands[3], cShadowBands[5], distance, share), fade);
                band = 3;
            }
        }
    }
    which = min(which, band);
    unitOnly = c;
    bandOnly = b;
    return min(c, b);
}

// The M2/WMO receivers' terminator fade: the shadow lifts as |N.L| drops (to 1 at 0.2), so back faces read lit.
float TerminatorFade(float3 n, float3 toLight)
{
    float t = 1.2 - abs(dot(n, toLight));
    t *= t;
    return saturate(t * t);
}

// A receiver that shaded itself with the cascades (normals.py: G-buffer a = 0.25 / 0.75; 0 / 1 for the rest and
// unwritten pixels). Without a G-buffer the host leaves shadowUnified off (c71.z 0).
bool SelfShadowed(float2 pixel)
{
    float a = tex2Dlod(sNormal, float4(pixel * DepthTexelSize(), 0, 0)).a;
    return a > 0.125 && a < 0.875;
}

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * LinearDepth(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

// shadowDebug, drawn with alpha blending over the scene: 1 the map read, at half strength; 2 the sun's visibility
// (shadowUnified pixels: the multiplier this pass lays on them, fog included, i.e. what the receiver shows);
// 3 magenta where no cascade is read (outside them all, or no world shader wrote the G-buffer: water, fixed function);
// 4-7 one term alone in grey, unlifted (terms.x land heightfield, y bands 0-2, z back-face rule, w unit map);
// 8 the caster's distance (shadowPenumbra / shadowMaxThrow, unified pixels).
float4 DebugView(float mode, float which, float visibility, float4 terms, float2 pixel, float distance, float share)
{
    // 8: the caster's distance back along the light (SoftCompare), grey to white at L4, red where shadowMaxThrow fades it
    [branch] if (mode > 7.5)
        return distance < 0 ? float4(0, 0, 0.3, 1) : float4(saturate(distance / cMaskSoftLevels.w).xxx * float3(1, share, share) + float3(1 - share, 0, 0), 1);
    [branch] if (mode > 3.5)
        return float4((mode < 4.5 ? terms.x : mode < 5.5 ? terms.y : mode < 6.5 ? terms.z : terms.w).xxx, 1);
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
                float fine = FineLit(sTerrainFine, uv);
                float2 from = cTerrainFineFrom.xy - cTerrainFineFrom.z * world.yx;
                [branch] if (cTerrainFineFrom.w < 1 && max(abs(from.x - 0.5), abs(from.y - 0.5)) < 0.5)
                    fine = lerp(FineLit(sTerrainFineFrom, from), fine, cTerrainFineFrom.w);
                lit = lerp(lit, fine, w);
            }
        }
        land = saturate(1 - (lit - world.z - cMaskAmbient.w) * cTerrainShadow.w);
    }

    float3 dx = Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0)));
    float3 dy = Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)));
    float3 n = cross(dx, dy);
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    n = normalize(ViewToWorldDirection(GBufferNormal(pixel, p, normalize(n))));
    float ndl = saturate(dot(n, cMaskLight.xyz));
    // The cascade term lifts at the terminator as the receivers' does. The land term does not: lifting it too (step 1)
    // erased a mountain's shadow on every surface under 0.2 N.L, all flat ground once the sun is under 11.5 deg, which
    // is when a mountain shades a valley, and left a tree's cascade shadow on that floor alone (2026-10-24). A back face
    // to the land's light is in its own shadow (land 0): taking the receivers' result there (step 2, land 1) left a
    // mountain's dark side to the cascades, which shade only where a ridge's back face stands before another (the
    // folded tops dark, the smooth foot not), and to the shading light, 6 deg higher than the shadows' at a low sun,
    // lighting slopes the shadows called dark (shadowvid video, 2026-10-24). At N.L 0 the target is the receivers' own
    // colour (cascade lifted; ndl under the shading light's 10 deg floor at most sin 6 deg), so the cut barely shows.
    float liftCascade = TerminatorFade(n, cMaskCascadeLight.xyz);
    float landHeights = land;
    float facing = dot(n, cMaskLandLight.xyz) <= 0 ? 0 : 1;
    land *= facing;
    bool unified = cMaskMaps.z > 0 && !SelfShadowed(pixel);
    [branch] if (land >= 1 && debug <= 0 && !unified)
        return 1;

    float which, unitOnly, bandOnly, distance, share;
    float cascade = lerp(ReceiverShadow(p, dx, dy, unified && cMaskSoft.z > 0, which, unitOnly, bandOnly, distance, share), 1, liftCascade);
    float3 lit = cMaskAmbient.rgb + cMaskDiffuse.rgb * ndl;
    float fog = saturate((length(p) - cMaskFog.x) * cMaskFog.y) * cMaskFog.z;
    [branch] if (unified)
    {
        // Both terms as a fraction of the receiver's own light (stock: lit x (1 - k (1 - S))), combined with min: a
        // tree's shadow inside a mountain's lands on the mountain's colour; the land term fades indoors and with
        // gfxShadowFade (c10.w), the cascade term with gfxShadowFade alone (k), as the receivers' did. One fog rule.
        float shadowed = 1 - cMaskDiffuse.w * (1 - liftCascade);
        float3 landRatio = lerp(1, lerp(min(cMaskAmbient.rgb / max(lit, 1.0 / 255), shadowed), 1, land), cMaskLight.w);
        float3 ratio = lerp(1, min(1 - cMaskDiffuse.w * (1 - cascade), saturate(landRatio)), 1 - fog);
        [branch] if (debug > 0)
            return DebugView(debug, which, dot(ratio, 1.0 / 3), float4(landHeights, bandOnly, facing, unitOnly), pixel, distance, share);
        return float4(ratio, 1);
    }
    [branch] if (debug > 0)
        return DebugView(debug, which, min(cascade, land), float4(landHeights, bandOnly, facing, unitOnly), pixel, distance, share);
    // Terrain's own min with its baked MCSH is not modelled: MCSH is per chunk, baked for another sun, and the depth
    // does not say which pixels are terrain, so MCSH-dark ground in the land's shadow still darkens twice.
    float3 stock = lit * (1 - cMaskDiffuse.w * (1 - cascade));
    // Shadows combine as min: in the land's shadow the target is the darker of the ambient and a full cascade shadow
    // (as lifted at the terminator), so a model's cascade shadow inside the land's shadow (stock < A, which this
    // multiply cannot lift) lands on the same colour as around it instead of showing through darker.
    float3 target = min(cMaskAmbient.rgb, lit * (1 - cMaskDiffuse.w * (1 - liftCascade)));
    float3 ratio = lerp(saturate(target / max(stock, 1.0 / 255)), 1, land);
    // The stock fog is in the colour when the volumetric fog did not push it out: fogged pixels keep their colour.
    return float4(lerp(1, saturate(ratio), cMaskLight.w * (1 - fog)), 1);
}
