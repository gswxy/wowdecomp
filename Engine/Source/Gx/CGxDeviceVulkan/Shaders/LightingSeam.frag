// BENILLA far view seam (Lighting.hlsl SeamMaskPs + SeamDepthPs in one, fog_composite.wgsl SEAM_FROM): drawn in the
// scene's pass with Present.vert's triangle, depth test ALWAYS + write, no colour. Pixels whose view depth lies in
// [gxSeam.x, gxSeam.y] are pushed to gxSeam.z (behind the horizon band) by a share smoothstep(x, y, depth) under a
// per-pixel IGN; gxDepth the scene depth copied just before.
#version 450
#extension GL_GOOGLE_include_directive : require
#define GX_FRAGMENT
#include "Lighting.glsl"

void main() {
    vec2 pos = gl_FragCoord.xy;
    float d = GxDepthAt(ivec2(pos));

    if (!GxIsGeometry(d)) {
        discard;
    }

    float w = GxClipW(GxViewPos(pos, d));

    if (w > gxSeam.y) {
        discard;
    }

    float k = smoothstep(gxSeam.x, gxSeam.y, w);
    float noise = fract(52.9829189 * fract(dot(pos, vec2(0.06711056, 0.00583715))));

    if (!(k > noise)) {
        discard;
    }

    gl_FragDepth = gxSeam.z;
}
