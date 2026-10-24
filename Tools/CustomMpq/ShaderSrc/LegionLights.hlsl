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
#include "vf_common.hlsli"
#include "vf_gbuffer.hlsli"

static const int kLights = 16;
float4 cLightControl : register(c10);
float4 cLights[kLights * 3] : register(c11);
float4 cLightShadow : register(c59);

sampler2D sDepth : register(s0);

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
        [branch] if (steps > 0 && max(c.r, max(c.g, c.b)) > 1.0 / 512)  // only lamps that reach the pixel
            c *= LightVisibility(p, n, cLights[i * 3].xyz, steps, jitter);
        light += c;
    }
    light *= cLightControl.x * fade;
    if (max(light.r, max(light.g, light.b)) < 1.0 / 512)
        discard;
    return float4(saturate(light), 1);
}
