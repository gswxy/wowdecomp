// Modern lighting post passes (port of Badgermilk0 D3d11Lighting.cpp s_postHlsl): SSAO, height fog, bloom, tonemap.
// Compiled offline by Lighting.py (vkd3d-compiler, as ffshaders/build.py) into LightingShaders.h. The Vulkan
// device runs the same passes as GLSL compute (CGxDeviceVulkan/Shaders/Lighting*.comp); keep the two in step.
cbuffer GxPost : register(b0) {
    row_major float4x4 gxInvProj;  // NDC to view
    row_major float4x4 gxInvView;  // view to world
    float4 gxViewportPx;           // the world viewport: x left, y top, z width, w height (pixels)
    float4 gxDepthRange;           // x min, y max, z 1 / (max - min)
    float4 gxTarget;               // this pass's target: x width, y height, z 1 / width, w 1 / height
    float4 gxAo;                   // x radius (yards), y intensity, z fade start, w fade end
    float4 gxAoProj;               // x pixels per yard at clip w 1
    float4 gxFogColor;             // rgb the engine's fog colour, a scattering
    float4 gxFogParams;            // x start, y end, z density, w 1 = the stock fog is on
    float4 gxSunColor;             // rgb
    float4 gxSunDir;               // xyz toward the sun (world)
    float4 gxHeightFog;            // x density, y falloff, z base height (world z), w most opacity
    float4 gxCamera;               // xyz camera (world), w time
    float4 gxBloom;                // x threshold, y knee, z intensity, w 1 = on
    float4 gxTone;                 // x exposure, y mode (0 clamp, 1 neutral, 2 ACES, 3 Reinhard, 4 Hable, 5 AgX, 6 AgX punchy, 7 PBR Neutral, 8 Uchimura, 9 Lottes)
    float4 gxSource;               // the texture sampled: xy 1 / size; zw the blur direction
    float4 gxProjW;                // clip w = dot(float4(view position, 1), gxProjW)
    float4 gxSsilvb;               // SSILVB: x AO strength, y indirect strength, z radius (yards), w thickness (yards)
    float4 gxSkyRow;               // sky occlusion: xy the map's lower corner (render-relative), z 1 / texel yd, w texels a side
    float4 gxSky;                  // x floor, y lift (yd), z lift per yd off the texel, w render origin z
    float4 gxWet;                  // rain on surfaces: x wetness 0..1, y 1 = screen-space reflections (0: the fog colour), z 1 = gxSkyMap is the shelter map
    float4 gxOrigin;               // xyz the render origin (world)
    row_major float4x4 gxProj;     // view to clip
    float4 gxSeam;                 // far view seam: x from (view depth), y to (farclip), z the depth a dissolved pixel takes
};

#ifdef GX_MSAA
Texture2DMS<float> gxDepth : register(t0);
float GxDepthAt(int2 p) {
    return gxDepth.Load(p, 0);
}
#else
Texture2D<float> gxDepth : register(t0);
float GxDepthAt(int2 p) {
    return gxDepth.Load(int3(p, 0));
}
#endif

Texture2D gxColor : register(t1);
Texture2D gxColor2 : register(t2);
SamplerState gxLinear : register(s0);

float3 GxDec(float3 c) {
    return pow(max(c, 0.0), 2.2);
}

float3 GxEnc(float3 c) {
    return pow(max(c, 0.0), 1.0 / 2.2);
}

// our D3D11 device applies no half-pixel offset (README: Half-pixel), so the pixel centre is the geometry's
float3 GxViewPos(float2 pixel, float depth) {
    float2 ndc = float2((pixel.x - gxViewportPx.x) / gxViewportPx.z * 2.0 - 1.0, 1.0 - (pixel.y - gxViewportPx.y) / gxViewportPx.w * 2.0);
    float4 h = mul(float4(ndc, (depth - gxDepthRange.x) * gxDepthRange.z, 1.0), gxInvProj);
    return h.xyz / h.w;
}

// the sky and the clear colour lie behind the world's depth range
bool GxIsGeometry(float depth) {
    return depth <= gxDepthRange.y + 0.00001 && depth < 0.99999;
}

int2 GxClampPixel(float2 p) {
    return int2(clamp(p, gxViewportPx.xy, gxViewportPx.xy + gxViewportPx.zw - 1.0));
}

float GxClipW(float3 p) {
    return dot(float4(p, 1.0), gxProjW);
}

struct GxPostVaryings {
    float4 pos : SV_Position;
};

GxPostVaryings FullscreenVs(uint id : SV_VertexID) {
    GxPostVaryings o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// SSAO at half resolution: x the unoccluded fraction, y the pixel's clip w (0 = sky)
float4 AoPs(GxPostVaryings i) : SV_Target0 {
    float2 pixel = gxViewportPx.xy + floor(i.pos.xy) * 2.0 + 0.5;
    float d = GxDepthAt(GxClampPixel(pixel));

    if (!GxIsGeometry(d)) {
        return float4(1.0, 0.0, 0.0, 0.0);
    }

    float3 p = GxViewPos(pixel, d);
    float w = GxClipW(p);
    float fade = 1.0 - saturate((w - gxAo.z) / max(gxAo.w - gxAo.z, 1.0));

    if (fade <= 0.0) {
        return float4(1.0, w, 0.0, 0.0);
    }

    // the normal from the nearer neighbour on each axis, so it does not bend over an edge
    float3 l = GxViewPos(pixel - float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel - float2(1.0, 0.0))));
    float3 r = GxViewPos(pixel + float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel + float2(1.0, 0.0))));
    float3 u = GxViewPos(pixel - float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel - float2(0.0, 1.0))));
    float3 b = GxViewPos(pixel + float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel + float2(0.0, 1.0))));
    float3 dx = dot(r - p, r - p) < dot(p - l, p - l) ? r - p : p - l;
    float3 dy = dot(b - p, b - p) < dot(p - u, p - u) ? b - p : p - u;
    float3 n = normalize(cross(dy, dx));
    n = dot(n, p) > 0.0 ? -n : n;

    float radius = gxAo.x;
    // the cap follows the screen: a fixed 96 shrank the radius at 4K to a fraction of a yard
    float radiusPx = clamp(radius * gxAoProj.x / max(w, 0.01), 3.0, gxViewportPx.w * 0.15);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    // two depth-buffer steps at this distance: a neighbour nearer than that may be the same surface, quantised
    float bias = 2.0 * gxAoProj.y * w * w;
    float sum = 0.0;
    const uint count = 12;

    [unroll] for (uint s = 0; s < count; s++) {
        float t = (s + 0.5) / count;
        float angle = (t * 7.0 + noise) * 6.2831853;
        float2 q = pixel + float2(cos(angle), sin(angle)) * (radiusPx * t);
        int2 qi = GxClampPixel(q);
        float3 v = GxViewPos(float2(qi) + 0.5, GxDepthAt(qi)) - p;
        float vv = dot(v, v);
        float range = saturate(1.0 - vv / (radius * radius * 4.0));
        sum += saturate((dot(v, n) - bias) * rsqrt(max(vv, 0.0001)) - 0.05) * range;
    }

    float ao = saturate(1.0 - gxAo.y * sum / count);
    return float4(lerp(1.0, ao, fade), w, 0.0, 0.0);
}

// a depth-aware Gaussian along gxSource.zw
float4 BlurPs(GxPostVaryings i) : SV_Target0 {
    int2 p = int2(i.pos.xy);
    float2 c = gxColor.Load(int3(p, 0)).xy;

    if (c.y <= 0.0) {
        return float4(1.0, 0.0, 0.0, 0.0);
    }

    float sum = c.x;
    float total = 1.0;
    int2 dir = int2(gxSource.zw);
    int2 last = int2(gxTarget.xy) - 1;

    [unroll] for (int k = -4; k <= 4; k++) {
        if (k == 0) {
            continue;
        }

        float2 s = gxColor.Load(int3(clamp(p + dir * k, int2(0, 0), last), 0)).xy;
        float weight = exp(-(k * k) / 8.0) * (s.y > 0.0 ? saturate(1.0 - abs(s.y - c.y) / (0.04 * c.y + 0.1)) : 0.0);
        sum += s.x * weight;
        total += weight;
    }

    return float4(sum / total, c.y, 0.0, 0.0);
}

// the occlusion into the scene, a multiply: upsampled from half resolution by depth
float4 AoCompositePs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));
    float w = GxClipW(GxViewPos(i.pos.xy, d));

    // lightingDebug 4: red geometry, green clip w / 200, blue the half-resolution occlusion under the pixel
    if (gxTone.z > 3.5) {
        float2 h4 = gxColor.Load(int3(int2((i.pos.xy - gxViewportPx.xy) * 0.5), 0)).xy;
        return float4(GxIsGeometry(d) ? 1.0 : 0.0, gxTone.z > 4.5 ? d : saturate(w / 200.0), h4.x, 1.0);
    }

    if (!GxIsGeometry(d)) {
        return 1.0;
    }
    float2 h = (i.pos.xy - gxViewportPx.xy - 0.5) * 0.5;
    int2 base = int2(floor(h));
    float2 f = h - base;
    int2 last = int2(1.0 / gxSource.xy + 0.5) - 1;
    float sum = 0.0;
    float total = 0.0;

    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            float2 s = gxColor.Load(int3(clamp(base + int2(x, y), int2(0, 0), last), 0)).xy;
            float bilinear = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            float weight = bilinear * (s.y > 0.0 ? exp(-abs(s.y - w) / (0.05 * w + 0.1)) : 0.0) + bilinear * 0.0001;
            sum += s.x * weight;
            total += weight;
        }
    }

    return float4((sum / max(total, 0.00001)).xxx, 1.0);
}

// height fog and the sun's scattering, blended over the scene by alpha
float4 FogPs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));

    if (!GxIsGeometry(d)) {
        discard;
    }

    float3 p = GxViewPos(i.pos.xy, d);
    float3 pw = mul(float4(p, 1.0), gxInvView).xyz;
    float3 ray = pw - gxCamera.xyz;
    float dist = length(ray);
    ray /= max(dist, 0.0001);

    // the stock fog's visibility here: the vertex shaders' formula on view z
    float visible = 1.0;

    if (gxFogParams.w > 0.0) {
        visible = min(pow(max((gxFogParams.y - p.z) / max(gxFogParams.y - gxFogParams.x, 0.001), 0.0), gxFogParams.z), 1.0);
    }

    float sun = pow(saturate(dot(ray, gxSunDir.xyz)), 8.0) * gxFogColor.a;
    float a1 = (1.0 - visible) * sun;
    float3 c1 = gxSunColor.rgb;
    float a2 = 0.0;
    float3 c2 = lerp(gxFogColor.rgb, gxSunColor.rgb, sun * 0.5);

    if (gxHeightFog.x > 0.0) {
        // exponential fog falling off with height, integrated from the camera to the pixel
        float k = gxHeightFog.y;
        float kd = k * (pw.z - gxCamera.z);
        float integral = gxHeightFog.x * exp(clamp(-k * (gxCamera.z - gxHeightFog.z), -40.0, 20.0)) * dist * (abs(kd) > 0.0001 ? (1.0 - exp(clamp(-kd, -40.0, 40.0))) / kd : 1.0);
        a2 = min(1.0 - exp(-max(integral, 0.0)), gxHeightFog.w);
    }

    float a = 1.0 - (1.0 - a1) * (1.0 - a2);

    if (a <= 0.0001) {
        discard;
    }

    return float4((c1 * (a1 * (1.0 - a2)) + c2 * a2) / a, a);
}

// Call of Duty's 13-tap downsample
float3 GxDown13(float2 uv, float2 t, bool decode) {
    static const float2 offsets[13] = { float2(-2, -2), float2(0, -2), float2(2, -2), float2(-1, -1), float2(1, -1), float2(-2, 0), float2(0, 0), float2(2, 0), float2(-1, 1), float2(1, 1), float2(-2, 2), float2(0, 2), float2(2, 2) };
    static const float weights[13] = { 0.03125, 0.0625, 0.03125, 0.125, 0.125, 0.0625, 0.125, 0.0625, 0.125, 0.125, 0.03125, 0.0625, 0.03125 };
    float3 sum = 0.0;

    [unroll] for (uint k = 0; k < 13; k++) {
        float3 c = gxColor.SampleLevel(gxLinear, uv + offsets[k] * t, 0.0).rgb;
        // the scene's own colour clamped, so one hot pixel does not flood the screen
        sum += (decode ? min(GxDec(c) * gxTone.x, 16.0) : c) * weights[k];
    }

    return sum;
}

float4 BloomPrefilterPs(GxPostVaryings i) : SV_Target0 {
    float3 c = GxDown13(i.pos.xy * gxTarget.zw, gxSource.xy, true);
    float bright = max(c.r, max(c.g, c.b));
    float knee = gxBloom.y;
    float soft = clamp(bright - gxBloom.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    return float4(c * (max(soft, bright - gxBloom.x) / max(bright, 0.00001)), 1.0);
}

float4 BloomDownPs(GxPostVaryings i) : SV_Target0 {
    return float4(GxDown13(i.pos.xy * gxTarget.zw, gxSource.xy, false), 1.0);
}

// a 3x3 tent, added into the larger level
float4 BloomUpPs(GxPostVaryings i) : SV_Target0 {
    float2 uv = i.pos.xy * gxTarget.zw;
    float2 t = gxSource.xy;
    float3 sum = gxColor.SampleLevel(gxLinear, uv, 0.0).rgb * 4.0;
    sum += (gxColor.SampleLevel(gxLinear, uv + float2(-t.x, 0.0), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(t.x, 0.0), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(0.0, -t.y), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += gxColor.SampleLevel(gxLinear, uv - t, 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + t, 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(t.x, -t.y), 0.0).rgb + gxColor.SampleLevel(gxLinear, uv + float2(-t.x, t.y), 0.0).rgb;
    return float4(sum / 16.0, 1.0);
}

float3 GxNeutral(float3 c) {
    // the stock colour up to the shoulder, then a curve that approaches white
    const float shoulder = 0.8;
    float m = max(c.r, max(c.g, c.b));

    if (m <= shoulder) {
        return c;
    }

    float mapped = shoulder + (1.0 - shoulder) * (1.0 - exp(-(m - shoulder) / (1.0 - shoulder)));
    return c * (mapped / m);
}

float3 GxAces(float3 c) {
    return saturate(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14));
}

// the curves below return linear [0, 1] like GxAces; TonemapPs encodes after
float3 GxReinhard(float3 c) {
    // extended Reinhard on luminance, white at 4
    const float white = 4.0;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float m = l * (1.0 + l / (white * white)) / (1.0 + l);
    return saturate(c * (m / max(l, 1e-6)));
}

float3 GxHableCurve(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

float3 GxHable(float3 c) {
    // Uncharted 2 filmic: exposure bias 2, white 11.2
    return saturate(GxHableCurve(2.0 * c) / GxHableCurve(float3(11.2, 11.2, 11.2)));
}

float3 GxAgx(float3 c, bool punchy) {
    // AgX (Troy Sobotka), Benjamin Wrensch's sixth-order fit; the rows of mul(v, M) are the GLSL columns
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 x = clamp(log2(max(mul(c, inset), 1e-10)), minEv, maxEv);
    x = (x - minEv) / (maxEv - minEv);
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;

    if (punchy) {
        float luma = dot(x, float3(0.2126, 0.7152, 0.0722));
        x = luma + 1.4 * (pow(max(x, 0.0), 1.35) - luma);
    }

    // the fit is display encoded (2.2)
    return pow(saturate(mul(x, outset)), 2.2);
}

float3 GxPbrNeutral(float3 c) {
    // Khronos PBR Neutral, the reference implementation
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));

    if (peak < startCompression) {
        return saturate(c);
    }

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return saturate(lerp(c, newPeak, g));
}

float3 GxUchimura(float3 x) {
    // Gran Turismo (Uchimura 2017), the default parameters
    const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
    float l0 = (P - m) * l / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = a * P / (P - S1);
    float CP = -C2 / P;
    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(m + l0, x);
    float3 w1 = 1.0 - w0 - w2;
    float3 T = m * pow(x / m, c) + b;
    float3 S = P - (P - S1) * exp(CP * (x - S0));
    float3 L = m + a * (x - m);
    return saturate(T * w0 + L * w1 + S * w2);
}

float3 GxLottes(float3 x) {
    // Lottes 2016, the default parameters
    const float a = 1.6, d = 0.977, hdrMax = 8.0, midIn = 0.18, midOut = 0.267;
    float ad = a * d;
    float b = (-pow(midIn, a) + pow(hdrMax, a) * midOut) / ((pow(hdrMax, ad) - pow(midIn, ad)) * midOut);
    float c = (pow(hdrMax, ad) * pow(midIn, a) - pow(hdrMax, a) * pow(midIn, ad) * midOut) / ((pow(hdrMax, ad) - pow(midIn, ad)) * midOut);
    return saturate(pow(x, a) / (pow(x, ad) * b + c));
}

float3 GxTonemap(float3 c, float mode) {
    int m = int(mode + 0.5);

    switch (m) {
    case 0: return saturate(c);
    case 1: return GxNeutral(c);
    case 2: return GxAces(c);
    case 3: return GxReinhard(c);
    case 4: return GxHable(c);
    case 5: return GxAgx(c, false);
    case 6: return GxAgx(c, true);
    case 7: return GxPbrNeutral(c);
    case 8: return GxUchimura(c);
    default: return GxLottes(c);
    }
}

float4 TonemapPs(GxPostVaryings i) : SV_Target0 {
    float4 s = gxColor.Load(int3(i.pos.xy, 0));
    float3 c = GxDec(s.rgb) * gxTone.x;

    if (gxBloom.w > 0.0) {
        // the chain adds every level into the one above: its mean, times the intensity
        c += gxColor2.SampleLevel(gxLinear, i.pos.xy * gxSource.xy, 0.0).rgb * (gxBloom.z / gxSource.z);
    }

    // no NaN or negative light reaches the frame
    c = max((asuint(c) & 0x7FFFFFFFu) > 0x7F800000u ? 0.0 : c, 0.0);

    return float4(GxEnc(GxTonemap(c, gxTone.y)), s.a);
}


// BENILLA auto-exposure (benilla-bourbon shaders/modern/meter.wgsl): the scene before any exposure, averaged into a
// 64x36 grid of log2 luminance (one pixel a cell of the R32_FLOAT target); the CPU takes the grid's mean.
// gxSource.xy 1 / the scene's size. The grid covers the world viewport only (outside it the scene is cleared black).
float4 MeterPs(GxPostVaryings i) : SV_Target0 {
    // taps a side in each cell; bilinear, so a cell reads a 2x box around each
    const uint taps = 4;
    // the floor under a black pixel: ten stops below the target, so one cannot sink the mean
    const float lumaFloor = 0.0001;
    float2 cell = floor(i.pos.xy);
    float sum = 0.0;

    [unroll] for (uint j = 0; j < taps; j++) {
        [unroll] for (uint k = 0; k < taps; k++) {
            float2 tap = (float2(k, j) + 0.5) / taps;
            float2 uv = (gxViewportPx.xy + (cell + tap) / gxTarget.xy * gxViewportPx.zw) * gxSource.xy;
            float3 c = GxDec(gxColor.SampleLevel(gxLinear, uv, 0.0).rgb);
            sum += log2(max(dot(c, float3(0.2126, 0.7152, 0.0722)), lumaFloor));
        }
    }

    return float4(sum / (taps * taps), 0.0, 0.0, 1.0);
}

// BENILLA SSILVB (benilla-bourbon shaders/ssilvb.wgsl): screen-space indirect lighting with a visibility bitmask.
// While a slice is swept, each sample's angular span (front face to a back face one thickness deeper) is OR-ed into a
// 32-sector mask of the hemisphere around the normal: AO = 1 - popcount(mask) / 32, and the bounce is what shut the
// NEWLY set sectors, in its colour. Full resolution over the world viewport, then a separable depth-aware 5-tap blur
// and a multiply into the frame. Our frame holds gamma values (benilla's "classic" frame), so the composite factor is
// raised to 1/2.2. Convention-free in view space: distances are clip w, the slice direction is measured, not assumed.
// SSILVB_AO_ONLY: the variant while the indirect strength is zero (2 slices x 4 steps, no colour path).
#ifdef SSILVB_AO_ONLY
#define SV_SLICES 2
#define SV_STEPS 4
#else
#define SV_SLICES 3
#define SV_STEPS 8
#endif

float SvFastAcos(float x) {
    // Eberly, max error ~1e-3 rad
    float a = abs(x);
    float r = (-0.156583 * a + 1.5707963) * sqrt(max(0.0, 1.0 - a));
    return x >= 0.0 ? r : 3.14159265 - r;
}

// countbits is SM5; the passes are ps_4_x
uint SvPopcount(uint x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
}

bool SvInView(int2 q) {
    return all(q >= int2(gxViewportPx.xy)) && all(q < int2(gxViewportPx.xy + gxViewportPx.zw));
}

float3 SvPos(int2 q) {
    return GxViewPos(float2(q) + 0.5, GxDepthAt(q));
}

// one axis of the depth gradient: the neighbour nearer in depth wins, a sky neighbour is not one
float3 SvAxisDelta(float3 p, float wp, int2 lo, int2 hi) {
    bool loOk = GxIsGeometry(GxDepthAt(lo));
    bool hiOk = GxIsGeometry(GxDepthAt(hi));

    if (!loOk && !hiOk) {
        return 0.0;
    }

    if (!loOk) {
        return SvPos(hi) - p;
    }

    if (!hiOk) {
        return p - SvPos(lo);
    }

    float3 a = SvPos(lo);
    float3 b = SvPos(hi);
    return abs(GxClipW(a) - wp) < abs(GxClipW(b) - wp) ? p - a : b - p;
}

float3 SvNormal(int2 q, float3 p) {
    float wp = GxClipW(p);
    float3 du = SvAxisDelta(p, wp, GxClampPixel(q - int2(1, 0)), GxClampPixel(q + int2(1, 0)));
    float3 dv = SvAxisDelta(p, wp, GxClampPixel(q - int2(0, 1)), GxClampPixel(q + int2(0, 1)));
    float3 n = cross(dv, du);

    if (dot(n, n) < 1e-12) {
        return normalize(-p);   // degenerate: face the camera and shut nothing
    }

    n = normalize(n);
    return dot(n, -p) < 0.0 ? -n : n;
}

// every sector between two normalised horizon angles; spans under 1/64 of a sector are dropped, not rounded up
uint SvSectorBits(float lo, float hi) {
    int span = int(ceil((hi - lo) * 32.0 - 1.0 / 64.0));

    if (span <= 0) {
        return 0u;
    }

    uint width = min(uint(span), 32u);
    uint start = min(uint(lo * 32.0), 31u);
    return (0xFFFFFFFFu >> (32u - width)) << start;
}

float3 SvSceneLinear(int2 q) {
    return GxDec(saturate(gxColor.Load(int3(q, 0)).rgb));
}

// the gather: rgb the bounce as a ratio to the receiver's brightness, a the visibility. gxColor the frame (indirect only)
float4 SsilvbPs(GxPostVaryings i) : SV_Target0 {
    const float pi = 3.14159265;
    float2 pixel = gxViewportPx.xy + floor(i.pos.xy) + 0.5;
    int2 px = int2(pixel);
    float d = GxDepthAt(px);

    if (!GxIsGeometry(d)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float3 p = GxViewPos(pixel, d);
    float3 n = SvNormal(px, p);
    float3 v = normalize(-p);
    float wp = GxClipW(p);
    float radius = gxSsilvb.z;
    float thickness = gxSsilvb.w;
    float range = radius * 1.5;     // RANGE_SCALE
    float rangeSq = range * range;

    // the world radius projected; faded out below ~24 px (a far hillside's own triangles otherwise draw in AO)
    float radiusPxRaw = gxAoProj.x * radius / max(wp, 1e-3);
    float fade = smoothstep(3.0, 24.0, radiusPxRaw);

    if (fade <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float radiusPx = clamp(radiusPxRaw, 3.0, 192.0);
    // interleaved gradient noise of the pixel alone, and its golden-ratio turn for the step offset
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    float noiseStep = frac(noise + 0.6180339887);
    float visibility = 0.0;
    float3 radiance = 0.0;

    [loop] for (int slice = 0; slice < SV_SLICES; slice++) {
        float phi = (slice + noise) * pi / SV_SLICES;
        float2 omega = float2(cos(phi), sin(phi));
        // the slice plane holds the view ray and the screen step: measured, so no axis convention is assumed
        float3 dir = normalize(GxViewPos(pixel + omega, d) - p);
        float3 axis = normalize(cross(dir, v));
        float3 tangent = cross(axis, n);

        if (dot(tangent, tangent) < 1e-10) {
            visibility += 1.0;  // the slice sees the surface edge on
            continue;
        }

        uint mask = 0u;

        [unroll] for (int side = 0; side < 2; side++) {
            float s = side == 1 ? -1.0 : 1.0;

            [loop] for (int tap = 0; tap < SV_STEPS; tap++) {
                float t = (tap + noiseStep) / SV_STEPS;
                float reach = max(1.5, radiusPx * t * t);
                int2 spx = int2(floor(pixel + s * omega * reach));

                if (!SvInView(spx)) {
                    continue;
                }

                float sd = GxDepthAt(spx);

                if (!GxIsGeometry(sd)) {
                    continue;   // the sky shuts nothing
                }

                float3 sp = GxViewPos(float2(spx) + 0.5, sd);
                float3 delta = sp - p;
                float distSq = dot(delta, delta);

                if (distSq < 1e-8 || distSq > rangeSq) {
                    continue;
                }

                float3 toSample = delta * rsqrt(distSq);
                float3 back = normalize(delta - v * thickness);
                float sideSign = dot(toSample, tangent) >= 0.0 ? 1.0 : -1.0;
                float2 horizon = float2(SvFastAcos(clamp(dot(toSample, n), -1.0, 1.0)), SvFastAcos(clamp(dot(back, n), -1.0, 1.0)));
                horizon = saturate(0.5 + sideSign * horizon / pi);
                uint bits = SvSectorBits(min(horizon.x, horizon.y), max(horizon.x, horizon.y));
                uint fresh = bits & ~mask;
                mask |= bits;

#ifndef SSILVB_AO_ONLY
                // only what this sample shut first lights us, and only a face the receiver is not behind
                bool behindSlab = wp - GxClipW(sp) > thickness;

                if (gxSsilvb.y > 0.0 && fresh != 0u && !behindSlab) {
                    float nDotL = saturate(dot(n, toSample));

                    if (nDotL > 0.0 && dot(SvNormal(spx, sp), -toSample) > 0.0) {
                        radiance += SvSceneLinear(spx) * (SvPopcount(fresh) / 32.0 * nDotL);
                    }
                }
#endif
            }
        }

        visibility += 1.0 - SvPopcount(mask) / 32.0;
    }

    float3 recv = SvSceneLinear(px);
    float peak = max(max(recv.r, max(recv.g, recv.b)), 0.005);   // RECEIVER_FLOOR
    return float4(radiance / SV_SLICES * fade / peak, lerp(1.0, visibility / SV_SLICES, fade));
}

// separable depth-aware 5-tap along gxSource.zw; the target is the viewport's size, depth is the frame's
float4 SsilvbBlurPs(GxPostVaryings i) : SV_Target0 {
    int2 t = int2(i.pos.xy);
    int2 last = int2(gxTarget.xy) - 1;
    int2 origin = int2(gxViewportPx.xy);
    float4 c = gxColor.Load(int3(t, 0));
    float d = GxDepthAt(origin + t);

    if (!GxIsGeometry(d)) {
        return c;
    }

    float z0 = GxClipW(GxViewPos(float2(origin + t) + 0.5, d));
    float tolerance = max(0.03 * z0, 0.02);
    int2 dir = int2(gxSource.zw);
    float4 sum = 0.0;
    float weight = 0.0;

    [unroll] for (int k = -2; k <= 2; k++) {
        int2 q = clamp(t + dir * k, int2(0, 0), last);
        float qd = GxDepthAt(origin + q);

        if (!GxIsGeometry(qd)) {
            continue;
        }

        float w = exp(-0.5 * k * k) * exp(-abs(GxClipW(GxViewPos(float2(origin + q) + 0.5, qd)) - z0) / tolerance);
        sum += gxColor.Load(int3(q, 0)) * w;
        weight += w;
    }

    return weight > 0.0 ? sum / weight : c;
}

// the factor multiplied into the frame (Dst x Src): occlusion + bounce, AO_GAIN 2, GI_GAIN 8, MAX_GAIN 4, ^(1/2.2) for
// the gamma frame. Above 1 (the bounce) only reaches a float frame (hdr 1); an 8-bit one clamps the factor at 1.
float4 SsilvbCompositePs(GxPostVaryings i) : SV_Target0 {
    float4 indirect = gxColor.Load(int3(int2(i.pos.xy - gxViewportPx.xy), 0));
    float occlusion = saturate(lerp(1.0, saturate(indirect.a), gxSsilvb.x * 2.0));
    float3 bounce = max(indirect.rgb, 0.0) * (gxSsilvb.y * 8.0);
    float3 factor = clamp(occlusion + bounce, 0.0, 4.0);
    return float4(pow(factor, 1.0 / 2.2), 1.0);
}

// BENILLA sky occlusion (benilla-bourbon sunshadow.wgsl sky_visibility + sky_ambient): the open-sky fraction over the
// pixel from the top-down map of the world's tops (t3), a 3x3 compare one texel apart (benilla: 4 texels of 0.5 yd),
// the pixel lifted so its own surface never counts as its roof. The frame x mix(floor, 1, open): benilla scales the
// exterior ambient in its gamma lanes by that; our post pass scales the gamma frame.
Texture2D<float> gxSkyMap : register(t3);

float4 SkyOcclusionPs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));

    if (!GxIsGeometry(d)) {
        discard;
    }

    float3 pw = mul(float4(GxViewPos(i.pos.xy, d), 1.0), gxInvView).xyz;
    float2 t = (pw.xy - gxSkyRow.xy) * gxSkyRow.z;
    float size = gxSkyRow.w;

    if (any(t < 1.0) || any(t > size - 1.0)) {
        discard;
    }

    float z = pw.z + gxSky.w + gxSky.y;
    float open = 0.0;

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float2 tap = floor(t) + float2(x, y);
            float off = length(tap + 0.5 - t) / gxSkyRow.z;
            open += gxSkyMap.Load(int3(tap, 0)) <= z + off * gxSky.z ? 1.0 : 0.0;
        }
    }

    // fades out over the map's outer eighth, so its edge never draws a line
    float2 edge = min(t, size - t) / (size * 0.125);
    float factor = lerp(gxSky.x, 1.0, open / 9.0);
    factor = lerp(1.0, factor, saturate(min(edge.x, edge.y)));
    return float4(factor, factor, factor, 1.0);
}

// BENILLA rain on surfaces (wet_hook.wgsl + wet_ssr.wgsl, weather/wetness.rs). benilla runs the hook in the receivers'
// shaders; ours is two post passes over the opaque frame (before the liquids): WetDarkenPs multiplies the frame by the
// wet albedo's ratio to the dry (wet_surface + the terrain's wet_puddles, on every surface), WetReflectPs adds the
// screen-space reflection weighted by WET_REFLECT_GAIN x wet_reflectance. The normal comes from the depth (SSAO's
// nearer-neighbour pair); the shelter is item 4's top-down map (a top 0.5-1.5 yd over the point covers it).
struct WetPoint {
    float3 p;      // view space
    float3 n;      // view space, toward the camera
    float w;       // wet_amount
    float boost;   // the gloss weight, > 1 standing water
    float puddle;
};

float WetHash(float2 p) {
    float2 q = frac(p * float2(0.1031, 0.1030));
    float2 r = q + dot(q, q.yx + 33.33);
    return frac((r.x + r.y) * r.x);
}

float WetNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(WetHash(i), WetHash(i + float2(1.0, 0.0)), u.x), lerp(WetHash(i + float2(0.0, 1.0)), WetHash(i + float2(1.0, 1.0)), u.x), u.y);
}

// shelter_amount: bilinear over the four texels around the point, each covered when its top lies 0.5-1.5 yd above
float WetOpen(float3 pw) {
    if (gxWet.z < 0.5) {
        return 1.0;
    }
    float2 g = (pw.xy - gxSkyRow.xy) * gxSkyRow.z - 0.5;
    float2 gf = floor(g);
    if (any(gf < 0.0) || any(gf + 1.0 > gxSkyRow.w - 1.0)) {
        return 1.0;
    }
    float z = pw.z + gxSky.w;
    float2 f = g - gf;
    float a = smoothstep(0.5, 1.5, gxSkyMap.Load(int3(gf, 0)) - z);
    float b = smoothstep(0.5, 1.5, gxSkyMap.Load(int3(gf + float2(1.0, 0.0), 0)) - z);
    float c = smoothstep(0.5, 1.5, gxSkyMap.Load(int3(gf + float2(0.0, 1.0), 0)) - z);
    float d = smoothstep(0.5, 1.5, gxSkyMap.Load(int3(gf + float2(1.0, 1.0), 0)) - z);
    return 1.0 - saturate(lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y));
}

bool WetAt(float2 pixel, out WetPoint o) {
    o = (WetPoint)0;
    float dep = GxDepthAt(int2(pixel));
    if (!GxIsGeometry(dep)) {
        return false;
    }
    o.p = GxViewPos(pixel, dep);
    float3 l = GxViewPos(pixel - float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel - float2(1.0, 0.0))));
    float3 r = GxViewPos(pixel + float2(1.0, 0.0), GxDepthAt(GxClampPixel(pixel + float2(1.0, 0.0))));
    float3 u = GxViewPos(pixel - float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel - float2(0.0, 1.0))));
    float3 b = GxViewPos(pixel + float2(0.0, 1.0), GxDepthAt(GxClampPixel(pixel + float2(0.0, 1.0))));
    float3 dx = dot(r - o.p, r - o.p) < dot(o.p - l, o.p - l) ? r - o.p : o.p - l;
    float3 dy = dot(b - o.p, b - o.p) < dot(o.p - u, o.p - u) ? b - o.p : o.p - u;
    o.n = normalize(cross(dy, dx));
    o.n = dot(o.n, o.p) > 0.0 ? -o.n : o.n;

    float3 pw = mul(float4(o.p, 1.0), gxInvView).xyz;
    float ny = mul(float4(o.n, 0.0), gxInvView).z;   // world up is z
    float exposure = WetOpen(pw);
    float wetness = saturate(gxWet.x);
    if (exposure <= 0.0) {
        return false;
    }
    // wet_amount / wet_surface's gloss
    o.w = wetness * exposure * smoothstep(-0.2, 0.25, ny) * lerp(0.5, 1.0, smoothstep(0.3, 0.8, ny));
    o.boost = o.w * smoothstep(0.35, 0.9, ny);
    // wet_puddles (world xy here is benilla's xz)
    {
        float2 xz = pw.xy + gxOrigin.xy;
        float field = 0.65 * WetNoise(xz * 0.17) + 0.35 * WetNoise(xz * 0.53 + 17.0);
        float level = smoothstep(0.94, 0.99, ny);
        float edge = lerp(0.8, 0.6, smoothstep(0.4, 1.0, wetness));
        o.puddle = smoothstep(edge, edge + 0.06, field) * level * smoothstep(0.35, 0.8, wetness) * exposure;
        o.boost = max(o.boost, 2.0 * o.puddle);
    }
    return o.w > 0.0;
}

// x the frame's ratio wet / dry: saturation x (1 + 0.3 w), x (1 - 0.36 w), puddles x (1 - 0.45 puddle)
float4 WetDarkenPs(GxPostVaryings i) : SV_Target0 {
    WetPoint wp;
    if (!WetAt(i.pos.xy, wp)) {
        discard;
    }
    if (gxTone.z == 7.0) {   // lightingDebug 7: r wetness, g puddle, b gloss weight / 2
        return float4(wp.w, wp.puddle, wp.boost * 0.5, 1.0);
    }
    float3 c = max(gxColor.Load(int3(i.pos.xy, 0)).rgb, 1.0 / 255.0);
    float luma = dot(c, float3(0.299, 0.587, 0.114));
    float3 wet = max(lerp(luma.xxx, c, 1.0 + 0.3 * wp.w), 0.0) * (1.0 - 0.36 * wp.w) * (1.0 - 0.45 * wp.puddle);
    return float4(saturate(wet / c), 1.0);
}

float2 WetUv(float3 q, out bool inside) {
    float4 clip = mul(float4(q, 1.0), gxProj);
    inside = false;
    if (clip.w <= 1e-4) {
        return 0.0;
    }
    float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    inside = all(uv >= 0.0) && all(uv <= 1.0);
    return uv;
}

float WetSceneW(float2 uv) {
    float2 pixel = gxViewportPx.xy + uv * gxViewportPx.zw;
    float d = GxDepthAt(GxClampPixel(pixel));
    return GxIsGeometry(d) ? GxClipW(GxViewPos(floor(pixel) + 0.5, d)) : 1.0e9;
}

// fs_reflect: the view ray mirrored about world up, 10 quadratic steps + 3 bisections against the depth, the frame
// snapshot (gxColor2, mip 2 = benilla's quarter blur off the puddles) where it lands, the fog colour where it leaves
float4 WetReflectPs(GxPostVaryings i) : SV_Target0 {
    WetPoint wp;
    if (!WetAt(i.pos.xy, wp) || wp.boost <= 0.0) {
        discard;
    }
    float3 v = -normalize(wp.p);
    float fres = 0.03 + 0.97 * pow(1.0 - saturate(dot(wp.n, v)), 5.0);
    float gloss = min(wp.boost, 1.0);
    float mirror = max(wp.boost - 1.0, 0.0);
    float tag = 0.55 * (gloss * min(0.25 * fres, 0.2) + mirror * (0.22 + 0.3 * fres));   // WET_REFLECT_GAIN x wet_reflectance
    if (tag < 1.0 / 256.0) {
        discard;
    }

    float3 up = normalize(mul(gxInvView, float4(0.0, 0.0, 1.0, 0.0)).xyz);   // world z in view space
    float3 r = reflect(-v, up);
    float3 fwd = normalize(gxProjW.xyz);
    float away = smoothstep(0.0, 0.25, dot(r, fwd));
    float3 sky = gxFogColor.rgb;
    if (away <= 0.0 || gxWet.y < 0.5) {
        return float4(sky * tag, 1.0);
    }

    float jitter = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    float d = GxClipW(wp.p);
    float reach = clamp(d * 2.0, 12.0, 400.0);
    float prevT = 0.0;
    float t = 0.0;
    bool hit = false;
    bool landed = false;
    float2 uv = (i.pos.xy - gxViewportPx.xy) / gxViewportPx.zw;
    float rz = max(abs(dot(r, fwd)), 0.1);
    [loop] for (int s = 0; s < 10; s++) {
        float k = (s + jitter) / 10.0;
        t = 0.3 + reach * k * k;
        float3 q = wp.p + r * t;
        bool inside;
        float2 suv = WetUv(q, inside);
        if (!inside) {
            break;
        }
        uv = suv;
        landed = true;
        float sd = WetSceneW(uv);
        float behind = GxClipW(q) - sd;
        float span = (t - prevT) * rz + 0.5 + 0.01 * sd;
        if (sd < 1.0e8 && behind > 0.0 && behind < span) {
            hit = true;
            break;
        }
        prevT = t;
    }
    if (hit) {
        float lo = prevT;
        float hi = t;
        [unroll] for (int k = 0; k < 3; k++) {
            float mid = 0.5 * (lo + hi);
            float3 q = wp.p + r * mid;
            bool inside;
            float2 suv = WetUv(q, inside);
            if (inside && GxClipW(q) > WetSceneW(suv)) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        bool inside;
        float2 suv = WetUv(wp.p + r * hi, inside);
        if (inside) {
            uv = suv;
        }
    }

    float3 refl = sky;
    if (landed) {
        float puddle = smoothstep(0.18, 0.35, tag);
        float2 src = (gxViewportPx.xy + uv * gxViewportPx.zw) * gxSource.xy;
        float3 fetched = lerp(gxColor2.SampleLevel(gxLinear, src, 2.0).rgb, gxColor2.SampleLevel(gxLinear, src, 0.0).rgb, puddle);
        float2 e = uv * 2.0 - 1.0;
        float2 e4 = e * e * e * e;
        float edge = saturate(1.0 - e4.x) * saturate(1.0 - e4.y);
        refl = lerp(sky, max(fetched, 0.0), edge * away);
    }
    return float4(refl * tag, 1.0);
}

// BENILLA far view seam (fog_composite.wgsl fs_composite, SEAM_FROM 0.7): over the last stretch before the view distance
// the detailed world dissolves into the band behind it. benilla blends toward a flat silhouette colour; here the share
// k = smoothstep(from, farclip, view depth) of the pixels (IGN dither, fixed per pixel) is pushed behind the horizon
// band, so FarLand's real ground (and the far trees) draw there instead. Pass 1: the mask from the depth.
float4 SeamMaskPs(GxPostVaryings i) : SV_Target0 {
    float d = GxDepthAt(int2(i.pos.xy));

    if (!GxIsGeometry(d)) {
        return 0.0;
    }

    float w = GxClipW(GxViewPos(i.pos.xy, d));

    if (w > gxSeam.y) {
        return 0.0;
    }

    float k = smoothstep(gxSeam.x, gxSeam.y, w);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    return k > noise ? 1.0 : 0.0;
}

// pass 2 (the frame's depth bound, colour writes off): a masked pixel takes the depth behind the horizon band's far
// land and in front of the sky's band
float SeamDepthPs(GxPostVaryings i) : SV_Depth {
    if (gxColor.Load(int3(i.pos.xy, 0)).r < 0.5) {
        discard;
    }

    return gxSeam.z;
}
