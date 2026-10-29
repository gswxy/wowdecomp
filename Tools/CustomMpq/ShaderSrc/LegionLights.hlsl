// LegionLights (ps_3_0, full res over the world viewport; Legion survey 2.8 + backlog #7, lightMode): the frame's
// M2 point lights (lamps, torches, braziers) on the surfaces around them, resolved from the scene depth in one
// screen-space pass and laid over the frame as colour += colour * light (GxBlend_ModAdd: dst * src + dst), so the
// frame's own colour is the albedo proxy: lit surfaces brighten in the lamp's colour and black stays black.
// Our own shader. Falloff after northlight's local_light_effects.hlsl (MIT, /root/wotlk-renderers/northlight):
// the light's own attenuation times a smooth window that reaches 0 at its radius; normals rebuilt from the depth as
// VFogShadowMask does. docs/LEGION_PORT.md "Legion lights".
// TEXCOORD0 = world-viewport uv. s0 scene depth. c0-c3 vf_common (c3.z = lightMaxDist), c10 (strength, 1 / fade
// span, fade start, 0), then 3 registers a light from c11: view position + 1 / radius^2, colour, attenuation c/l/q;
// c59 shadows (lightShadows): (darkness 0 = off, march length cap yd, occluder thickness yd, shadow distance yd).
// Shadows (2026-10-24, lightocc): per lit light a short ray from the pixel toward the lamp, marched in view space and
// read back through the depth buffer (Bend-style contact shadows, docs/shadow-research/03-screenspace-cheap.md 1a,
// as one SSAO-like tap loop): a sample behind the depth there by less than the thickness counts as blocked.
// Baked shadows (2026-10-24, lightbake): a static lamp's colour.w / attenuation.w carry its slot's texel origin
// (y, x; y -1 = none) in s1, the cube of ray hit distances / 40 yd cast once through the world's collision
// (LegionLights.cpp RunBake), c4-c7 view to world, c60 (1 / atlas w, 1 / atlas h, face texels, darkness 0 = off).
// Soft baked shadows (2026-10-26, benilla 8 stage 2, locallight.wgsl local_visibility): c61 = benilla's LightGrid.soft
// (taps, emitter radius yd, shade floor, 0); taps > 1 = blocker search + Vogel disc, contact-hardening.
#include "vf_common.hlsli"
#include "vf_gbuffer.hlsli"

static const int kLights = 16;
float4 cLightControl : register(c10);
float4 cLights[kLights * 3] : register(c11);
float4 cLightShadow : register(c59);

float4 cBake : register(c60);
float4 cSoft : register(c61);

sampler2D sDepth : register(s0);
sampler2D sBake : register(s1);

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * LinearDepth(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

static const float kShadowSteps = 12;
static const float kNearZ = 0.5;        // yd: a march toward a lamp behind the camera stops at this view depth
static const float kLampClearance = 0.75;   // yd: the march ends short of the lamp (its own fire, wood and holder)

// 1 = the lamp reaches p, down to 1 - darkness where the depth buffer blocks the ray. Off screen (the ray leaves
// the viewport) counts as open: nothing there to read.
float LightVisibility(float3 p, float3 n, float3 lamp, float steps, float jitter)
{
    float3 o = p + n * (0.05 + 0.004 * p.z);    // off the surface: no self hits on the receiver's own depth
    float3 dir = lamp - o;
    float len = length(dir);
    dir /= max(len, 1e-3);
    float end = min(len - kLampClearance, cLightShadow.y);
    if (dir.z < 0)
        end = min(end, (o.z - kNearZ) / -dir.z);
    if (end <= 0)
        return 1;
    float stepLen = end / steps;
    float blocked = 0;
    [loop] for (float i = 0; i < kShadowSteps; i++)
    {
        if (i >= steps)
            break;
        float3 q = o + dir * ((i + jitter) * stepLen);
        float2 tap = NdcToPixel(q.xy / q.z * ViewToNdcScale() + ViewToNdcOffset());
        if (any(tap < ViewportOrigin()) || any(tap > ViewportLastPixelCentre() + 0.5))
            break;
        float gap = q.z - LinearDepth(SampleDepth(sDepth, tap));
        float bias = 0.03 + 0.003 * q.z;
        // soft both ways: ramps in over 0.25 yd past the bias, out over the last half of the thickness
        blocked = max(blocked, saturate((gap - bias) * 4) * saturate((cLightShadow.z - gap) * 2 / cLightShadow.z));
    }
    return 1 - blocked * cLightShadow.x;
}

// One cube texel's hit distance (fraction of the radius) of the slot at origin; face texel t clamped to the face.
float BakeTap(float2 origin, float face, float2 t)
{
    t = clamp(t, 0, cBake.z - 1);
    float2 texel = origin + float2(face * cBake.z + t.x, t.y) + 0.5;
    return tex2Dlod(sBake, float4(texel * cBake.xy, 0, 0)).r;
}

// 1 = the baked world lets the lamp reach p (view space), down to 1 - darkness; 2x2 PCF over the cube face.
// Faces as RayDir: major axis a, sign face & 1, u along axis a + 1, v along a + 2.
static const float kBakeRange = 40;     // yd: LegionLights.cpp kMaxRadius, a texel's 254 (255 = no hit)

// locallight.wgsl: BLOCKER_TAPS, MAX_TAPS, the penumbra clamp in face texels
static const int kBlockerTaps = 6;
static const int kMaxTaps = 12;
static const float kMaxPenumbraTexels = 12;
static const float kMinPenumbraTexels = 1;

// locallight.wgsl vogel_offset (r in 0..1 over the disc)
float2 VogelOffset(float index, float count, float rotation)
{
    float r = sqrt((index + 0.5) / count);
    float theta = index * 2.399963229728653 + rotation;
    return float2(r * cos(theta), r * sin(theta));
}

// locallight.wgsl local_visibility's soft path on the baked cube: stored radial hit distances (yd) instead of
// reverse-Z depth, compared against the same reach as the hard path. world = the receiver in world space.
float SoftBakedVisibility(float2 origin, float face, float2 t, float reach, float L, float major, float3 world)
{
    float3 wrapped = frac(world * 0.1);
    float rotation = frac(dot(wrapped, float3(12.9898, 78.233, 37.719))) * 6.2831853;
    float toYd = kBakeRange * 255.0 / 254.0;
    float blocker = 0;
    float blockers = 0;
    [loop] for (int i = 0; i < kBlockerTaps; ++i)
    {
        float stored = BakeTap(origin, face, floor(t + 0.5 + VogelOffset(i, kBlockerTaps, rotation + 1.3) * kMaxPenumbraTexels)) * toYd;
        if (stored < reach)
        {
            blocker += stored;
            blockers += 1;
        }
    }
    if (blockers <= 0)
        return 1;
    float occluder = max(blocker / blockers, 0.25);   // CUBE_NEAR
    float penumbra = cSoft.y * max(L - occluder, 0) / occluder;   // radial: the bake stores ray lengths
    // a yard at distance major is 0.5 / major of the face's [0,1] uv, cBake.z texels across
    float spread = clamp(penumbra * 0.5 / max(major, 1e-3) * cBake.z, kMinPenumbraTexels, kMaxPenumbraTexels);
    float lit = 0;
    [loop] for (int j = 0; j < kMaxTaps; ++j)
        lit += BakeTap(origin, face, floor(t + 0.5 + VogelOffset(j, kMaxTaps, rotation) * spread)) * toYd >= reach;
    float visibility = lit / kMaxTaps;
    return lerp(1 - cSoft.z, 1, visibility);
}

float BakedVisibility(float3 p, float3 n, float3 lamp, float2 origin)
{
    float3 w = mul(float4(p + n * 0.15 - lamp, 0), cViewToWorld).xyz;
    float3 aw = abs(w);
    float L = length(w);
    float face;
    float2 uv;
    if (aw.x >= aw.y && aw.x >= aw.z)
    {
        face = w.x < 0 ? 1 : 0;
        uv = w.yz / aw.x;
    }
    else if (aw.y >= aw.z)
    {
        face = w.y < 0 ? 3 : 2;
        uv = w.zx / aw.y;
    }
    else
    {
        face = w.z < 0 ? 5 : 4;
        uv = w.xy / aw.z;
    }
    float2 t = (uv * 0.5 + 0.5) * cBake.z - 0.5;
    float2 t0 = floor(t);
    float2 f = t - t0;
    // the bias grows with the distance: a texel spans about 2 / face texels of a radian
    float reach = L - 0.4 - L * 2.0 / cBake.z;
    if (cSoft.x > 1)
        return SoftBakedVisibility(origin, face, t, reach, L, max(aw.x, max(aw.y, aw.z)), mul(float4(p, 1), cViewToWorld).xyz);
    float4 lit = float4(BakeTap(origin, face, t0), BakeTap(origin, face, t0 + float2(1, 0)),
                        BakeTap(origin, face, t0 + float2(0, 1)), BakeTap(origin, face, t0 + 1)) * (kBakeRange * 255.0 / 254.0) >= reach;
    float open = lerp(lerp(lit.x, lit.y, f.x), lerp(lit.z, lit.w, f.x), f.y);
    return 1 - (1 - open) * cBake.w;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(viewportUv);
    float depth = SampleDepth(sDepth, pixel);
    if (depth > DeepestWorldDepth())    // sky and the far WDL horizon
        discard;
    float3 p = ViewRayAtUnitDepth(pixel) * LinearDepth(depth);
    float fade = saturate((cLightControl.z - length(p)) * cLightControl.y);
    if (fade <= 0)
        discard;

    float3 dx = Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0)));
    float3 dy = Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)));
    float3 n = cross(dx, dy);
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    n = GBufferNormal(pixel, p, normalize(n));

    // Shadow rays: 12 taps near, down to 6 by 100 yd, none past the shadow distance; one jitter a pixel.
    float steps = cLightShadow.x > 0 && p.z < cLightShadow.w ? clamp(14 - p.z * 0.08, 6, kShadowSteps) : 0;
    float jitter = InterleavedGradientNoise(pixel);

    // Literal register indices only (no relative addressing for the translators); unused slots carry colour 0.
    float3 light = 0;
    [unroll] for (int i = 0; i < kLights; ++i)
    {
        float3 v = cLights[i * 3].xyz - p;
        float d2 = dot(v, v);
        float d = sqrt(d2);
        float3 att = cLights[i * 3 + 2].xyz;
        float window = saturate(1 - d2 * cLights[i * 3].w);
        float falloff = min(1 / max(att.x + att.y * d + att.z * d2, 1e-3), 1.5) * window * window;
        float ndl = saturate(dot(n, v) / max(d, 1e-3));
        float3 c = cLights[i * 3 + 1].rgb * (falloff * ndl);
        [branch] if (max(c.r, max(c.g, c.b)) > 1.0 / 512)  // only lamps that reach the pixel
        {
            float vis = 1;
            if (steps > 0)
                vis = LightVisibility(p, n, cLights[i * 3].xyz, steps, jitter);
            if (cBake.w > 0 && cLights[i * 3 + 1].w >= 0)
                vis = min(vis, BakedVisibility(p, n, cLights[i * 3].xyz, float2(cLights[i * 3 + 2].w, cLights[i * 3 + 1].w)));
            c *= vis;
        }
        light += c;
    }
    light *= cLightControl.x * fade;
    if (max(light.r, max(light.g, light.b)) < 1.0 / 512)
        discard;
    return float4(saturate(light), 1);
}
