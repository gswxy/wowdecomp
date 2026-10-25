"""Terrain height blending (D3D11 and Vulkan; D3D9 keeps the stock bytecode).

Shared by both translators: this Vulkan one (GLSL) and shader-xlate's sm3hlsl.py (HLSL, which imports this module).
The stock Terrain1 / Terrain2[_pcf] / Terrain3[_pcf] pixel shaders read layer k (k < L) through sampler k at its own
texcoord, and the chunk's blend texture through sampler L (xyz the alpha of layers 1..3, w the baked shadow). Here
the PS takes the blend texture once (`gxTpA`, which then stands in for the body's own read) and, with height
blending on, re-weights the layers by their height maps (s10 + k, sampled at the layers' own texcoords; sand settles
between cobbles) into gxTpA.xyz.

Pixel constants (MapRenderChunk.cpp TerrainPomBind, set for every chunk draw; 0 = stock): c13 = 1 per layer that
has a height map; c14.x the height blend (0-1). Height maps hold the height in .a, as retail's _h.blp do; the client
makes them at run time from the loaded diffuse (WorldClient/TerrainHeightGen.*). The parallax march that also lived
here was removed 2026-10-25 (it did not look right).
"""
import re

TERRAIN_PS = ('terrain1', 'terrain2', 'terrain2_pcf', 'terrain3', 'terrain3_pcf')
HEIGHT_SLOT = 10


def _base(name):
    b = str(name).replace('\\', '/').split('/')[-1].split(':')[0].lower()
    return b[:-4] if b.endswith('.bls') else b


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


# The pixel code, in GLSL; hlsl() rewrites it. TPK(i) pixel constant, TPH(k, uv, dx, dy) layer k's height,
# BLEND the blend-texture read, U0 layer 0's texcoord variable.
_PROLOGUE = '''
    // terrain height blend (bls/terrainpom.py)
    vec4 gxTpA = BLEND;
    {
        vec2 gxDx = dFdx(U0);
        vec2 gxDy = dFdy(U0);
        vec4 gxPrm = TPK(14);
        if (gxPrm.x > 0.0 && LAYERS > 1) {
            vec4 gxHas = TPK(13) * MASK;
            vec4 gxW = vec4(0.0, gxTpA.x, gxTpA.y, gxTpA.z) * MASK;
            gxW.x = 1.0 - clamp(gxW.y + gxW.z + gxW.w, 0.0, 1.0);
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
'''


def prologue(L, reads, lang):
    """The lines that go after the input copies. reads: find_reads()."""
    uv = [reads[k][1] for k in range(L)]
    heights = ['TPH(%d, %s, gxDx, gxDy)' % (k, uv[k]) if k < L else '0.0' for k in range(4)]
    t = (_PROLOGUE.replace('BLEND', reads[L][0])
         .replace('MASK', 'vec4(%s)' % ', '.join('1.0' if k < L else '0.0' for k in range(4)))
         .replace('HEIGHTS', 'vec4(%s)' % ', '.join(heights))
         .replace('LAYERS', str(L))
         .replace('U0', uv[0]))
    return _lang(t, lang).split('\n')


def rewrite_body(lines, L, reads):
    """The body with the blend read replaced by gxTpA."""
    return [ln.replace(reads[L][0], 'gxTpA') for ln in lines]


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
        return t.replace('TPK(13)', 'regs.p[13]').replace('TPK(14)', 'regs.p[14]')
    t = t.replace('TPK(13)', 'c[13]').replace('TPK(14)', 'c[14]')
    for a, b in (('vec2', 'float2'), ('vec4', 'float4'), ('mix(', 'lerp('), ('dFdx(', 'ddx('), ('dFdy(', 'ddy(')):
        t = t.replace(a, b)
    return t


if __name__ == '__main__':
    # self-check: a 2-layer read set gives one prologue in each language with unshifted heights and no stray tokens
    reads = {0: ('texture(s0, v2.xy)', 'v2.xy'), 1: ('texture(s1, v3.wz)', 'v3.wz'), 2: ('texture(s2, v4.xy)', 'v4.xy')}
    assert layers(['Terrain1:2'], {0: '2d', 1: '2d', 2: '2d'}) == 2
    assert layers(['Terrain1:3'], {0: 'cube', 1: '2d', 2: '2d'}) == 0
    assert layers(['Terrain3_pcf:44'], {0: '2d', 1: '2d', 5: '2d', 6: '2d'}) == 1
    assert layers(['TerrainSM:0'], {0: '2d', 1: '2d'}) == 0
    assert find_reads('r0 = texture(s1, v3.wz); r1 = texture(s0, v2.xy); r2 = texture(s2, v4.xy);', 2,
                      r'texture\(s(\d+), (v\d+\.[xyzw]{2})\)') == reads
    assert rewrite_body(['r0 = texture(s1, v3.wz) * texture(s2, v4.xy);'], 2, reads) == \
        ['r0 = texture(s1, v3.wz) * gxTpA;']
    for lang in ('glsl', 'hlsl'):
        txt = '\n'.join(prologue(2, reads, lang))
        assert 'gxTpH(1, v3.wz, gxDx, gxDy)' in txt and 'gxOff' not in txt and 'gxT.' not in txt
        assert not re.search(r'TPK|TPH|BLEND|MASK|HEIGHTS|LAYERS', txt), txt
        assert ('float4' in txt) == (lang == 'hlsl')
    print('ok')
