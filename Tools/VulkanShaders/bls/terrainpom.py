"""Terrain height blending and parallax occlusion mapping (D3D11 and Vulkan; D3D9 keeps the stock bytecode).

Shared by both translators: this Vulkan one (GLSL) and shader-xlate's sm3hlsl.py (HLSL, which imports this module).
The stock Terrain1 / Terrain2[_pcf] / Terrain3[_pcf] pixel shaders read layer k (k < L) through sampler k at its own
texcoord, and the chunk's blend texture through sampler L (xyz the alpha of layers 1..3, w the baked shadow). Here:

- the terrain VS also writes gxT (varying location 15 / TEXCOORD14): the view-space position, w = 1;
- the PS takes the blend texture once (`gxTpA`, which then stands in for the body's own read), finds the dominant
  layer, marches its height map (s10 + k, `_h.blp`) along the view ray for 8-16 steps chosen from the view angle and
  the mip level, with the polygon at the map's mean height (so relief rises and sinks around it), refines with one secant step, and shifts every layer's texcoord by the result; then, with height
  blending on, re-weights the layers by height (sand settles between cobbles) into gxTpA.xyz.

Pixel constants (MapRenderChunk.cpp TerrainPomConstants, set for every chunk draw; 0 = stock): c13 = 1 per layer that
has a height map; c14 = x height blend (0-1), y parallax depth in yards, z height-map texels per texcoord unit, w the
mip level where the parallax has faded out. Height maps hold the height in .a, as retail's _h.blp do: Data/patch-T.MPQ
(wxl-light's tools/make_heightmaps.py, 2026-10-16: 1291 maps, 128 px DXT5, one per stock tileset texture; retail has
_h.blp only for MoP-and-later tilesets, none of which a 3.3.5a map uses).
The march is planar: texcoords are a linear map of world xy (the stock VS), height is along world up (VS c2 in view
space), so there is no tangent frame and no seam between triangles.
"""
import re

TERRAIN_PS = ('terrain1', 'terrain2', 'terrain2_pcf', 'terrain3', 'terrain3_pcf')
HEIGHT_SLOT = 10
LOCATION = 15          # Vulkan varying location of gxT
SEMANTIC = 'TEXCOORD14'


def _base(name):
    b = str(name).replace('\\', '/').split('/')[-1].split(':')[0].lower()
    return b[:-4] if b.endswith('.bls') else b


def is_terrain_vs(names):
    return any(_base(n) == 'terrain' for n in names)


def layers(names, samplers):
    """L when every name is a stock terrain pixel shader whose samplers 0..L are all 2D (L layers, blend at L);
    0 otherwise (env/cube permutations, other shaders)."""
    if not names or any(_base(n) not in TERRAIN_PS for n in names):
        return 0
    low = sorted(s for s in samplers if s < 5)
    if not low or low != list(range(len(low))) or any(samplers[s] not in ('2d', 2) for s in low):
        return 0
    return len(low) - 1 if len(low) >= 2 else 0


def find_reads(body, L, sample_re):
    """{slot: (expr, uv expr)} for the layer and blend reads in the body, or None when any is missing or ambiguous.
    sample_re matches one sampling expression with groups (slot, uv expr): `texture(s2, v3.wz)`, the Terrain2/3
    permutations pack two texcoords in one register."""
    found = {}
    for m in re.finditer(sample_re, body):
        slot, var = int(m.group(1)), m.group(2)
        if slot > L:
            continue
        if slot in found and found[slot] != (m.group(0), var):
            return None
        found[slot] = (m.group(0), var)
    return found if all(k in found for k in range(L + 1)) else None


# The pixel code, in GLSL; hlsl() rewrites it. TPK(i) pixel constant, TPV(i) vertex constant, GXT the varying,
# TPH(k, uv, dx, dy) layer k's height, BLEND the blend-texture read, Uk layer k's texcoord variable.
_PROLOGUE = '''
    // terrain height blend and parallax (bls/terrainpom.py)
    vec4 gxTpA = BLEND;
    vec2 gxOff = vec2(0.0, 0.0);
    {
        vec2 gxDx = dFdx(U0);
        vec2 gxDy = dFdy(U0);
        vec3 gxPx = dFdx(GXT.xyz);
        vec3 gxPy = dFdy(GXT.xyz);
        vec4 gxPrm = TPK(14);
        if (gxPrm.x + gxPrm.y > 0.0 && GXT.w > 0.5) {
            vec4 gxHas = TPK(13) * MASK;
            vec4 gxW = vec4(0.0, gxTpA.x, gxTpA.y, gxTpA.z) * MASK;
            gxW.x = 1.0 - clamp(gxW.y + gxW.z + gxW.w, 0.0, 1.0);
            vec4 gxWh = gxW * gxHas;
            int gxDom = 0;
            float gxMax = gxWh.x;
            float gxSec = 0.0;
            if (gxWh.y > gxMax) { gxSec = gxMax; gxDom = 1; gxMax = gxWh.y; } else { gxSec = max(gxSec, gxWh.y); }
            if (gxWh.z > gxMax) { gxSec = gxMax; gxDom = 2; gxMax = gxWh.z; } else { gxSec = max(gxSec, gxWh.z); }
            if (gxWh.w > gxMax) { gxSec = gxMax; gxDom = 3; gxMax = gxWh.w; } else { gxSec = max(gxSec, gxWh.w); }
            // the mip level of the height map: the parallax fades out over the two levels before gxPrm.w
            float gxLod = 0.5 * log2(max(max(dot(gxDx, gxDx), dot(gxDy, gxDy)) * gxPrm.z * gxPrm.z, 1e-8));
            // and towards the line where another layer takes over as dominant, so the shift does not jump there
            float gxFade = (1.0 - smoothstep(gxPrm.w - 2.0, gxPrm.w, gxLod)) * clamp((gxMax - gxSec) * 4.0, 0.0, 1.0);
            // planar: texcoord = A * world xy, height along world up; A from the screen derivatives with A * up = 0
            vec3 gxUp = normalize(TPV(2).xyz);
            vec3 gxR = normalize(GXT.xyz);
            float gxK = -dot(gxR, gxUp);
            vec3 gxC0 = cross(gxPy, gxUp);
            vec3 gxC1 = cross(gxUp, gxPx);
            float gxDet = dot(gxPx, gxC0);
            if (gxPrm.y > 0.0 && gxFade > 0.01 && gxK > 0.02 && abs(gxDet) > 1e-14) {
                // texcoord shift per yard of depth along the view ray
                vec2 gxDir = (gxDx * dot(gxC0, gxR) + gxDy * dot(gxC1, gxR)) / (gxDet * max(gxK, 0.25));
                int gxN = clamp(int(mix(8.0, 16.0, (1.0 - gxK) * gxFade) + 0.5), 8, 16);
                float gxLayer = 1.0 / float(gxN);
                vec2 gxStep = gxDir * (gxPrm.y * gxFade * gxLayer);
                vec2 gxBase = DOMUV;
                // the polygon is the map's mean height (its last mip), not its top: the ray enters the top plane
                // that much before it, so the ground lifts and sinks around the polygon instead of only sinking
                vec2 gxCur = -gxStep * (float(gxN) * (1.0 - TPH(gxDom, gxBase, gxDx * 4096.0, gxDy * 4096.0)));
                float gxD = 0.0;
                float gxH = 1.0 - TPH(gxDom, gxBase + gxCur, gxDx, gxDy);
                float gxPrevD = 0.0;
                float gxPrevH = gxH;
                gxOff = gxCur;
                int gxTaken = 0;
                for (int gxI = 0; gxI < 16; gxI++) {
                    if (gxI >= gxN || gxD >= gxH) {
                        break;
                    }
                    gxPrevD = gxD;
                    gxPrevH = gxH;
                    gxCur += gxStep;
                    gxD += gxLayer;
                    gxH = 1.0 - TPH(gxDom, gxBase + gxCur, gxDx, gxDy);
                    gxTaken = gxI + 1;
                }
                if (gxTaken > 0) {
                    // secant step between the last two depths
                    float gxAfter = gxH - gxD;
                    float gxBefore = gxPrevH - gxPrevD;
                    float gxFr = gxAfter < 0.0 ? clamp(gxBefore / max(gxBefore - gxAfter, 1e-5), 0.0, 1.0) : 1.0;
                    gxOff = gxCur - gxStep * (1.0 - gxFr);
                }
            }
            if (gxPrm.x > 0.0 && LAYERS > 1) {
                // retail-style height blend: alpha times height, kept within reach of the tallest, renormalised;
                // a layer without a height map counts as half height
                vec4 gxHt = HEIGHTS;
                gxHt = gxHas * gxHt + (vec4(1.0, 1.0, 1.0, 1.0) - gxHas) * 0.5;
                vec4 gxB = gxW * (gxHt + vec4(0.05, 0.05, 0.05, 0.05));
                float gxBm = max(max(gxB.x, gxB.y), max(gxB.z, gxB.w));
                gxB = max(gxB - vec4(gxBm, gxBm, gxBm, gxBm) * 0.65, vec4(0.0, 0.0, 0.0, 0.0));
                gxB = gxB / max(gxB.x + gxB.y + gxB.z + gxB.w, 1e-5);
                gxB = mix(gxW, gxB, clamp(gxPrm.x, 0.0, 1.0));
                gxTpA.xyz = gxB.yzw;
            }
        }
    }
'''


def prologue(L, reads, lang):
    """The lines that go after the input copies. reads: find_reads()."""
    uv = [reads[k][1] for k in range(L)]
    dom = uv[0]
    for k in range(1, L):
        dom = '(gxDom == %d ? %s : %s)' % (k, uv[k], dom)
    heights = ['TPH(%d, %s + gxOff, gxDx, gxDy)' % (k, uv[k]) if k < L else '0.0' for k in range(4)]
    t = (_PROLOGUE.replace('BLEND', reads[L][0])
         .replace('MASK', 'vec4(%s)' % ', '.join('1.0' if k < L else '0.0' for k in range(4)))
         .replace('DOMUV', dom)
         .replace('HEIGHTS', 'vec4(%s)' % ', '.join(heights))
         .replace('LAYERS', str(L))
         .replace('U0', uv[0]))
    return _lang(t, lang).split('\n')


def rewrite_body(lines, L, reads):
    """The body with every layer read shifted by gxOff and the blend read replaced by gxTpA."""
    subs = [(reads[k][0], reads[k][0].replace(reads[k][1], '%s + gxOff' % reads[k][1], 1)) for k in range(L)]
    subs.append((reads[L][0], 'gxTpA'))
    out = []
    for ln in lines:
        for a, b in subs:
            ln = ln.replace(a, b)
        out.append(ln)
    return out


def height_fn(L, lang):
    """The function TPH(k, uv, dx, dy) calls: layer k's height map (s10 + k), .a."""
    if lang == 'glsl':
        s = ['float gxTpH(int k, vec2 uv, vec2 dx, vec2 dy) {']
        for k in range(L):
            s.append('    if (k == %d) { return textureGrad(s%d, uv, dx, dy).a; }' % (k, HEIGHT_SLOT + k))
    else:
        s = ['float gxTpH(int k, float2 uv, float2 dx, float2 dy) {']
        for k in range(L):
            s.append('    if (k == %d) { return t%d.SampleGrad(s%d, uv, dx, dy).a; }' % (k, HEIGHT_SLOT + k, HEIGHT_SLOT + k))
    s += ['    return 0.5;', '}']
    return s


def _lang(t, lang):
    t = t.replace('TPH(', 'gxTpH(')
    if lang == 'glsl':
        return t.replace('TPK(', 'regs.p[').replace('TPV(', 'regs.v[').replace('GXT', 'gxT') \
            .replace('regs.p[13)', 'regs.p[13]').replace('regs.p[14)', 'regs.p[14]').replace('regs.v[2)', 'regs.v[2]')
    t = t.replace('TPK(13)', 'c[13]').replace('TPK(14)', 'c[14]').replace('TPV(2)', 'VC[2]').replace('GXT', 'I.gxT')
    for a, b in (('vec2', 'float2'), ('vec3', 'float3'), ('vec4', 'float4'), ('mix(', 'lerp('), ('dFdx(', 'ddx('),
                 ('dFdy(', 'ddy(')):
        t = t.replace(a, b)
    return t.replace('for (int gxI', '[loop] for (int gxI')


def vs_line(pos, lang):
    """The terrain VS line that writes gxT (pos: the position input register)."""
    if lang == 'glsl':
        return '    gxT = vec4((regs.v[0] * {0}.x + regs.v[1] * {0}.y + regs.v[2] * {0}.z + regs.v[3]).xyz, 1.0);'.format(pos)
    return '    O.gxT = float4((c[0] * {0}.x + c[1] * {0}.y + c[2] * {0}.z + c[3]).xyz, 1.0);'.format(pos)


if __name__ == '__main__':
    # self-check: a 2-layer read set gives one prologue in each language with the uv shifts and no stray tokens
    reads = {0: ('texture(s0, v2.xy)', 'v2.xy'), 1: ('texture(s1, v3.wz)', 'v3.wz'), 2: ('texture(s2, v4.xy)', 'v4.xy')}
    assert layers(['Terrain1:2'], {0: '2d', 1: '2d', 2: '2d'}) == 2
    assert layers(['Terrain1:3'], {0: 'cube', 1: '2d', 2: '2d'}) == 0
    assert layers(['Terrain3_pcf:44'], {0: '2d', 1: '2d', 5: '2d', 6: '2d'}) == 1
    assert layers(['TerrainSM:0'], {0: '2d', 1: '2d'}) == 0
    assert find_reads('r0 = texture(s1, v3.wz); r1 = texture(s0, v2.xy); r2 = texture(s2, v4.xy);', 2,
                      r'texture\(s(\d+), (v\d+\.[xyzw]{2})\)') == reads
    assert rewrite_body(['r0 = texture(s1, v3.wz) * texture(s2, v4.xy);'], 2, reads) == \
        ['r0 = texture(s1, v3.wz + gxOff) * gxTpA;']
    for lang in ('glsl', 'hlsl'):
        txt = '\n'.join(prologue(2, reads, lang))
        assert 'gxOff = gxCur' in txt and 'v3.wz + gxOff' in txt
        assert not re.search(r'TPK|TPV|TPH|GXT|BLEND|MASK|DOMUV|SHIFT|HEIGHTS|LAYERS', txt), txt
        assert ('float4' in txt) == (lang == 'hlsl')
    print('ok')
