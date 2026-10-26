// BENILLA SSILVB gather (Lighting.hlsl SvFastAcos .. SsilvbPs, benilla ssilvb.wgsl): the body of LightingSsilvb.comp and
// (SSILVB_AO_ONLY) LightingSsilvbAo.comp. Over the world viewport into a viewport-sized RGBA16F image: rgb the bounce
// as a ratio to the receiver's brightness, a the visibility. gxColor the scene (read by the AO-only variant too, for
// the receiver: its rgb is 0 there either way). ipos is the D3D11 SV_Position of the viewport-sized target.
#include "Lighting.glsl"

layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gxOut;

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

vec3 SvSceneLinear(ivec2 q) {
    return GxDec(clamp(texelFetch(gxColor, q, 0).rgb, 0.0, 1.0));
}

// the gather: rgb the bounce as a ratio to the receiver's brightness, a the visibility. gxColor the frame (indirect only)
vec4 Ssilvb(vec2 ipos) {
    const float pi = 3.14159265;
    vec2 pixel = gxViewportPx.xy + floor(ipos) + 0.5;
    ivec2 px = ivec2(pixel);
    float d = GxDepthAt(px);

    if (!GxIsGeometry(d)) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    vec3 p = GxViewPos(pixel, d);
    vec3 n = SvNormal(px, p);
    vec3 v = normalize(-p);
    float wp = GxClipW(p);
    float radius = gxSsilvb.z;
    float thickness = gxSsilvb.w;
    float range = radius * 1.5;     // RANGE_SCALE
    float rangeSq = range * range;

    // the world radius projected; faded out below ~24 px (a far hillside's own triangles otherwise draw in AO)
    float radiusPxRaw = gxAoProj.x * radius / max(wp, 1e-3);
    float fade = smoothstep(3.0, 24.0, radiusPxRaw);

    if (fade <= 0.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    float radiusPx = clamp(radiusPxRaw, 3.0, 192.0);
    // interleaved gradient noise of the pixel alone, and its golden-ratio turn for the step offset
    float noise = fract(52.9829189 * fract(dot(ipos, vec2(0.06711056, 0.00583715))));
    float noiseStep = fract(noise + 0.6180339887);
    float visibility = 0.0;
    vec3 radiance = vec3(0.0);

    for (int slice = 0; slice < SV_SLICES; slice++) {
        float phi = (float(slice) + noise) * pi / float(SV_SLICES);
        vec2 omega = vec2(cos(phi), sin(phi));
        // the slice plane holds the view ray and the screen step: measured, so no axis convention is assumed
        vec3 dir = normalize(GxViewPos(pixel + omega, d) - p);
        vec3 axis = normalize(cross(dir, v));
        vec3 tangent = cross(axis, n);

        if (dot(tangent, tangent) < 1e-10) {
            visibility += 1.0;  // the slice sees the surface edge on
            continue;
        }

        uint mask = 0u;

        for (int side = 0; side < 2; side++) {
            float s = side == 1 ? -1.0 : 1.0;

            for (int tap = 0; tap < SV_STEPS; tap++) {
                float t = (float(tap) + noiseStep) / float(SV_STEPS);
                float reach = max(1.5, radiusPx * t * t);
                ivec2 spx = ivec2(floor(pixel + s * omega * reach));

                if (!SvInView(spx)) {
                    continue;
                }

                float sd = GxDepthAt(spx);

                if (!GxIsGeometry(sd)) {
                    continue;   // the sky shuts nothing
                }

                vec3 sp = GxViewPos(vec2(spx) + 0.5, sd);
                vec3 delta = sp - p;
                float distSq = dot(delta, delta);

                if (distSq < 1e-8 || distSq > rangeSq) {
                    continue;
                }

                vec3 toSample = delta * inversesqrt(distSq);
                vec3 back = normalize(delta - v * thickness);
                float sideSign = dot(toSample, tangent) >= 0.0 ? 1.0 : -1.0;
                vec2 horizon = vec2(SvFastAcos(clamp(dot(toSample, n), -1.0, 1.0)), SvFastAcos(clamp(dot(back, n), -1.0, 1.0)));
                horizon = clamp(0.5 + sideSign * horizon / pi, 0.0, 1.0);
                uint bits = SvSectorBits(min(horizon.x, horizon.y), max(horizon.x, horizon.y));
                uint fresh = bits & ~mask;
                mask |= bits;

#ifndef SSILVB_AO_ONLY
                // only what this sample shut first lights us, and only a face the receiver is not behind
                bool behindSlab = wp - GxClipW(sp) > thickness;

                if (gxSsilvb.y > 0.0 && fresh != 0u && !behindSlab) {
                    float nDotL = clamp(dot(n, toSample), 0.0, 1.0);

                    if (nDotL > 0.0 && dot(SvNormal(spx, sp), -toSample) > 0.0) {
                        radiance += SvSceneLinear(spx) * (float(bitCount(fresh)) / 32.0 * nDotL);
                    }
                }
#endif
            }
        }

        visibility += 1.0 - float(bitCount(mask)) / 32.0;
    }

    vec3 recv = SvSceneLinear(px);
    float peak = max(max(recv.r, max(recv.g, recv.b)), 0.005);   // RECEIVER_FLOOR
    return vec4(radiance / float(SV_SLICES) * fade / peak, mix(1.0, visibility / float(SV_SLICES), fade));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);

    if (any(greaterThanEqual(id, ivec2(gxViewportPx.zw)))) {
        return;
    }

    imageStore(gxOut, id, Ssilvb(vec2(id) + 0.5));
}
