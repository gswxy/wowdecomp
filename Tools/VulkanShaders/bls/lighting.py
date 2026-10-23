"""Per-pixel lighting (Badgermilk0's "modern lighting", the shader half; Engine/Source/Gx/GxLighting.h): the
lit families' vertex modules hand their view-space surface to the pixel module (gxN / gxP / gxM / gxF at
locations 11..14), which lights COLOR0 (and a WMO's specular COLOR1) again per pixel with the stock formulas,
the hemisphere ambient and the point lights of the GxLighting block (set 1, binding 1).

GLSL port of his D3d11Dx9Shader.cpp (s_hlslLighting, Writer::SurfaceOutputs, the LitPixel splice,
FamilyForShader): same math. No clusters: GxDynamic loops the gxLightFlags.y lights of the flat array. The
pixel module relights only when gxLightFlags.x != 0, regs.misc.y > 0.5 (the device: the bound vertex module
carries `flag lit_surface`), gxF.x > 0.5 and gxP.w > 0.999. With gxLightFlags.x == 0 every module takes the
stock path. GXK(i) is the vertex constant i, regs.v[i], in both stages (the pixel module's v[] holds the
paired vertex module's rows).
"""

FAMILY_NONE, FAMILY_M2, FAMILY_M2ENV, FAMILY_WMO, FAMILY_WMO_SPEC, FAMILY_WMO_COMP, FAMILY_WMO_COMP_SPEC, \
    FAMILY_TERRAIN, FAMILY_LITPIXEL = range(9)

VARYINGS = (('gxN', 11), ('gxP', 12), ('gxM', 13), ('gxF', 14))


def _family_one(name, vertex):
    """His FamilyForShader, case-insensitive, on one engine name (`Name` or `Name:perm`)."""
    base = name.split(':')[0].lower()
    if vertex:
        if base == 'terrain':
            return FAMILY_TERRAIN
        if base.startswith('mapobj'):
            comp = base.startswith('mapobju')
            if 'specular' in base:
                return FAMILY_WMO_COMP_SPEC if comp else FAMILY_WMO_SPEC
            return FAMILY_WMO_COMP if comp else FAMILY_WMO
        if base.startswith(('diffuse_', 'cdiffuse_', 'color_')):
            return FAMILY_M2ENV if 'env' in base else FAMILY_M2
        return FAMILY_NONE
    if base.startswith(('combiners_', 'mapobj')) or (base.startswith('terrain') and base != 'terrainsm'
                                                     and not base.startswith('terrain1w')):
        return FAMILY_LITPIXEL
    return FAMILY_NONE


def families(names, vertex):
    """The families of a folded record's names. A record is lit only when every name is of a lit family and
    they all produce the same text (identical bytes under MapObjSpecular_T1 and MapObjUSpecular_T1, say, whose
    unlit permutations come out the same)."""
    return {_family_one(n, vertex) for n in names}


BLOCK = '''
// Per-pixel lighting (GxLighting.h GxLightingGpu, std140; Tools/VulkanShaders/bls/lighting.py).
struct GxLight {
    vec3 pos;        // view space
    float radius;    // no light from here on
    vec3 color;      // linear
    float start;     // full light up to here
    float flicker;   // amplitude, 0 to 1
    float seed;
    float specular;
    float pad;
};

layout(std140, set = 1, binding = 1) uniform GxLighting {
    uvec4 gxLightFlags;              // x GX_LIGHT_* (0 = off), y lights, z lightRoughnessBias (float bits), w lightingDebug
    layout(row_major) mat4 gxView;   // render-relative world to view (row vector * matrix), for the terrain
    layout(row_major) mat4 gxViewInv;
    vec4 gxSkyTint;                  // rgb sky colour (luminance 1), a hemisphere strength
    vec4 gxGroundTint;               // rgb ground colour (luminance 1), a WMO normal map strength (0 = off)
    vec4 gxUpView;                   // xyz world up in view space, w time in seconds
    vec4 gxLightParams;              // x wrap, y dynamic light intensity, z M2 specular, w WMO specular
    GxLight gxLights[32];
    vec4 gxArmor;                    // PBR-lite armour: x plate metalness, y plate roughness, z mail metalness, w mail roughness
};

#define GXK(i) regs.v[i]
#define GX_LIGHT_PERPIXEL 1u
#define GX_LIGHT_DYNAMIC 2u
#define GX_LIGHT_SPECULAR 4u
#define GX_LIGHT_HEMISPHERE 8u
#define GX_LIGHT_PBR 0x200u

// gxF.x: the family in bits 0-2, the stock point lights in 3-5, 64 a normal, 128 WMO specular, 256 sphere-mapped
// M2, 512 a WMO that adds its vertex colour to the lit material (MapObjUDiffuse_Comp). gxF.yzw: the terrain's
// lighting scale (0.5, or the vertex colour)
#define GX_SURF_M2 1u
#define GX_SURF_WMO 2u
#define GX_SURF_TERRAIN 3u

vec3 GxDec(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 GxEnc(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }

// the hemisphere ambient: the zone's sky colour from above, its ground colour from below, the stock level
vec3 GxHemi(vec3 n, bool on) {
    if (!on) {
        return vec3(1.0);
    }
    float up = clamp(dot(n, gxUpView.xyz) * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(1.0), mix(gxGroundTint.rgb, gxSkyTint.rgb, up), gxSkyTint.a);
}

// C17-C20 / C21-C24 the stock point lights' colours and positions, C25-C27 their constant, linear and quadratic
// attenuation (M2 and WMO)
vec3 GxStockPointLights(vec3 n, vec3 p, uint lights) {
    vec3 l = vec3(0.0);
    for (int k = 0; k < 4; k++) {
        if (uint(k) < lights) {
            vec3 d = GXK(21 + k).xyz - p;
            float d2 = dot(d, d);
            float inv = inversesqrt(abs(d2));
            float att = 1.0 / (GXK(25)[k] + GXK(26)[k] * (d2 * inv) + GXK(27)[k] * d2);
            l += GXK(17 + k).rgb * (att * max(dot(d, n) * inv, 0.0));
        }
    }
    return l;
}

// M2 (Diffuse_*): C10 sun colour, C11 ambient, C12 sun direction
vec3 GxStockM2(vec3 n, vec3 p, uint lights, bool hemi) {
    return clamp(GXK(10).rgb * clamp(dot(-GXK(12).xyz, n), 0.0, 1.0) + GXK(11).rgb * GxHemi(n, hemi) + GxStockPointLights(n, p, lights), 0.0, 1.0);
}

// WMO (MapObjDiffuse_*): C10 ambient, C11 sun colour, C12 sun direction
vec3 GxStockWmo(vec3 n, vec3 p, uint lights, bool hemi) {
    return clamp(GXK(11).rgb * clamp(dot(-GXK(12).xyz, n), 0.0, 1.0) + GXK(10).rgb * GxHemi(n, hemi) + GxStockPointLights(n, p, lights), 0.0, 1.0);
}

// MapObjSpecular_*: C13 colour and power
vec3 GxStockWmoSpecular(vec3 n, vec3 p) {
    vec3 h = normalize(normalize(p) + GXK(12).xyz);
    float s = clamp(dot(n, -h), 0.0, 1.0);
    return (s == 0.0 ? 0.0 : exp2(GXK(13).w * log2(s))) * GXK(13).rgb;
}

// the terrain: world normal and position, C24 direction to the sun, C25 ambient, C26 sun colour; C28-C36 three
// point lights (position through the chunk's rotation, colour, constant / linear / quadratic attenuation)
vec3 GxStockTerrain(vec3 nw, vec3 pw, vec3 n, uint lights, bool hemi) {
    vec3 l = GXK(26).rgb * clamp(dot(GXK(24).xyz, nw), 0.0, 1.0) + GXK(25).rgb * GxHemi(n, hemi);
    for (int k = 0; k < 3; k++) {
        if (uint(k) < lights) {
            vec4 at = GXK(28 + k * 3);
            vec3 d = (at.x * GXK(0).xyz + at.y * GXK(1).xyz + at.z * GXK(2).xyz) - pw;
            float inv = inversesqrt(abs(dot(d, d)));
            float dist = 1.0 / inv;
            vec4 att = GXK(30 + k * 3);
            l += clamp(dot(d * inv, nw), 0.0, 1.0) * GXK(29 + k * 3).rgb * (1.0 / (att.y * dist + att.x + dist * dist * att.z));
        }
    }
    return min(l, vec3(1.0));
}
'''

PIXEL_LIB = '''
// PBR-lite (lightPbr): GGX / Cook-Torrance, Smith-Schlick visibility, Schlick Fresnel. a = roughness^2, f0 grey;
// returns BRDF * N.L for a light from unit direction l
float GxGgx(vec3 n, vec3 v, vec3 l, float a, float f0) {
    float nl = clamp(dot(n, l), 0.0, 1.0);
    if (nl <= 0.0) {
        return 0.0;
    }
    vec3 h = normalize(l + v);
    float nh = clamp(dot(n, h), 0.0, 1.0);
    float vh = clamp(dot(v, h), 0.0, 1.0);
    float nv = max(dot(n, v), 1e-4);
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float k = a * 0.5;
    float vis = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
    float fr = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    return a2 / (3.14159265 * d * d) * vis * fr * nl;
}

// the PBR-lite material of this pixel, for the end of the pixel module: gxMetal tints the specular by the textured
// colour, gxMask takes the texture alpha as the specular mask (M2 items and sphere-mapped parts)
float gxMetal = 0.0;
float gxMask = 0.0;
float gxF0 = 0.04;       // the f0 the GGX was taken with
float gxMetalLum = 0.0;  // 1: metal only on texels bright enough (dark straps, wood and cloth stay dielectric)
float gxMetalGrey = 0.0; // 1: metal only on grey texels too (iron bands on a painted or wooden doodad, not the paint)

// ARMORMETAL: set in the stock body (shadow_capture): the sun's visibility from the shadow map (1 lit) and the
// body's first s0 texel (x < 0: none); gxUv0 the M2's texcoord0, for the composite skin's region
float gxShadow = 1.0;
vec4 gxTex0 = vec4(-1.0);
vec2 gxUv0 = vec2(-1.0);

// ARMORMETAL: the armour of the composite body skin texel at uv (GxLighting.h GxLightingMaterial_Body): 0 none, 1 mail,
// 2 plate, 2 bits per CCharacterComponent section (s_sectionInfoRaw on the 512 x 512 texture; the face has none)
uint GxSkinArmor(uint bits, vec2 uv) {
    if (bits == 0u || uv.x < 0.0) {
        return 0u;
    }
    vec2 t = clamp(uv, 0.0, 1.0);
    uint r;
    if (t.x < 0.5) {
        if (t.y >= 0.625) {
            return 0u;
        }
        r = t.y < 0.25 ? 0u : (t.y < 0.5 ? 1u : 2u);
    } else {
        r = t.y < 0.25 ? 3u : (t.y < 0.375 ? 4u : (t.y < 0.625 ? 5u : (t.y < 0.875 ? 6u : 7u)));
    }
    return (bits >> (2u * r)) & 3u;
}

// the point lights at view position p (all gxLightFlags.y of them): diffuse returned, specular (power > 0, or GGX
// when a > 0) in spec
vec3 GxDynamic(vec3 p, vec3 n, float power, out vec3 spec, float a, float f0) {
    spec = vec3(0.0);
    vec3 sum = vec3(0.0);
    if ((gxLightFlags.x & GX_LIGHT_DYNAMIC) == 0u) {
        return sum;
    }
    uint count = min(gxLightFlags.y, 32u);
    vec3 v = normalize(-p);
    float t = gxUpView.w;
    float wrap = gxLightParams.x;
    for (uint k = 0u; k < count; k++) {
        GxLight l = gxLights[k];
        vec3 d = l.pos - p;
        float dist = length(d);
        if (dist >= l.radius) {
            continue;
        }
        float s = clamp((l.radius - dist) / max(l.radius - l.start, 0.01), 0.0, 1.0);
        // a flame's slow waver (two incommensurate waves), not a fast strobe
        float flicker = 1.0 - l.flicker * (0.5 + 0.3 * sin(t * 3.1 + l.seed) + 0.2 * sin(t * 7.3 + l.seed * 1.7));
        vec3 c = l.color * (s * s * (3.0 - 2.0 * s) * flicker);
        vec3 dir = d / max(dist, 0.0001);
        float ndl = dot(n, dir);
        sum += c * clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
        if (a > 0.0) {
            spec += c * (GxGgx(n, v, dir, a, f0) * l.specular);
        } else if (power > 0.0 && ndl > 0.0) {
            vec3 h = normalize(dir + v);
            spec += c * (pow(clamp(dot(n, h), 0.0, 1.0), power) * l.specular * clamp(ndl * 4.0, 0.0, 1.0));
        }
    }
    return sum;
}

// the specular a sphere-mapped M2 (or any PBR-lite surface) adds to the pixel shader's colour: gxSpecular the
// dynamic lights' (their own attenuation), gxSpecSun the sun's (under gxShadow, as the stock shader shadows the sun's
// diffuse), gxSpecAmb the sky's reflection, never brighter than the ambient diffuse it takes (gxAmbCap = (1 - kd) / kd
// of the final colour; that colour is already shadowed)
vec3 gxSpecular = vec3(0.0);
vec3 gxSpecSun = vec3(0.0);
vec3 gxSpecAmb = vec3(0.0);
float gxAmbCap = 0.0;

// the specular at the end of the pixel module: a metal's tinted by the texel; under gxMask the texture alpha is the
// specular mask (unmasked texels keep a quarter, a dielectric's)
vec3 GxSpecFinal(vec4 o) {
    bool tex = gxTex0.x >= 0.0;
    float m = mix(1.0, clamp(tex ? gxTex0.a : o.a, 0.0, 1.0), gxMask);
    vec3 albedo = GxDec(tex ? gxTex0.rgb : o.rgb);
    float lum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    vec3 tint = lum > 0.001 ? min(albedo / lum, vec3(3.0)) : vec3(1.0);
    float hi = max(albedo.r, max(albedo.g, albedo.b));
    float sat = (hi - min(albedo.r, min(albedo.g, albedo.b))) / max(hi, 1e-3);
    float mm = mix(1.0, smoothstep(0.015, 0.06, lum), gxMetalLum) * mix(1.0, 1.0 - smoothstep(0.2, 0.45, sat), gxMetalGrey);
    vec3 t = mix(vec3(1.0), tint, gxMetal * m * mm) * (mix(0.25, 1.0, m) * mix(min(0.02 / max(gxF0, 0.04), 1.0), 1.0, mm));
    return (gxSpecSun * gxShadow + gxSpecular) * t + min(gxSpecAmb * t, GxDec(o.rgb) * gxAmbCap);
}

// WMONORMALS: a MapObj pixel's normal bent by its material's normal map (stage 10, CMapObj::BindNormalMap), the
// tangent frame from the screen derivatives of the view position and the uv (Schuler's cotangent frame, its sign
// taken from the screen's handedness). t is the texel: rg the slope along +u / +v (rows down), b up, *0.5+0.5.
// A null (0) or white (1) stage, a material without one, is no valid normal: w 0, the surface's own normal stays
vec4 GxWmoNormal(vec3 nv, vec3 dp1, vec3 dp2, vec2 du1, vec2 du2, vec4 t, float strength) {
    vec3 tn = t.xyz * 2.0 - 1.0;
    float len = dot(tn, tn);
    vec3 nn = normalize(nv);
    vec3 a = cross(dp2, nn);
    vec3 b = cross(nn, dp1);
    vec3 tu = a * du1.x + b * du2.x;
    vec3 tv = a * du1.y + b * du2.y;
    float m = max(dot(tu, tu), dot(tv, tv));
    if (tn.z <= 0.0 || len < 0.56 || len > 1.56 || m < 1e-30 || dot(nv, nv) < 1e-12) {
        return vec4(0.0);
    }
    float k = inversesqrt(m) * strength * (dot(cross(dp1, dp2), nn) < 0.0 ? -1.0 : 1.0);
    return vec4(normalize(tu * (k * tn.x) + tv * (k * tn.y) + nn * tn.z), 1.0);
}

// a lit family's pixel: COLOR0 (and a WMO's specular COLOR1) lit again per pixel, with the dynamic lights added in
// linear space. flatN is the face's normal, for a surface that has none; nmap.w 1: nmap.xyz the normal-mapped WMO
// normal (GxWmoNormal)
vec4 GxLightPixel(vec4 c0, inout vec4 c1, vec3 flatN, vec4 nmap) {
    uint f = uint(gxF.x + 0.5);
    uint family = f & 7u;
    uint lights = (f >> 3) & 7u;
    bool hasNormal = (f & 64u) != 0u;
    bool wmoSpecular = (f & 128u) != 0u;
    bool env = (f & 256u) != 0u;
    bool composite = (f & 512u) != 0u;
    bool valid = hasNormal && gxN.w > 0.999;

    // lightingDebug 2: green relit per pixel, red unlit on purpose, blue lit with the face normal
    if (gxLightFlags.w == 2u) {
        return vec4(valid ? 0.0 : (hasNormal ? 1.0 : 0.0), valid ? 1.0 : 0.0, hasNormal ? 0.0 : 1.0, c0.a);
    }

    // a surface with a normal whose colour is not the stock lighting is unlit on purpose: left alone
    if (hasNormal && !valid) {
        return c0;
    }

    vec3 p = gxP.xyz;
    vec3 n = hasNormal ? (nmap.w > 0.5 && family == GX_SURF_WMO ? nmap.xyz : normalize(gxN.xyz)) : flatN;
    uint features = gxLightFlags.x;
    bool hemi = (features & GX_LIGHT_HEMISPHERE) != 0u;
    vec4 c = c0;
    vec3 mat = vec3(1.0);

    if (valid) {
        mat = family == GX_SURF_TERRAIN ? gxF.yzw : (composite ? GXK(28).rgb : gxM.rgb);

        if ((features & (GX_LIGHT_PERPIXEL | GX_LIGHT_HEMISPHERE)) != 0u) {
            if (family == GX_SURF_M2) {
                c.rgb = clamp(gxM.rgb * GxStockM2(n, p, lights, hemi) + GXK(29).rgb, 0.0, 1.0);
            } else if (family == GX_SURF_WMO && composite) {
                c.rgb = clamp(GXK(28).rgb * GxStockWmo(n, p, lights, hemi) + gxM.rgb + GXK(29).rgb, 0.0, 1.0);
            } else if (family == GX_SURF_WMO) {
                c.rgb = clamp(gxM.rgb * GxStockWmo(n, p, lights, hemi) + GXK(29).rgb, 0.0, 1.0);
            } else {
                vec3 pw = p;   // the terrain's C0-C3 is world * view: its gxW is already view space
                c.rgb = GxStockTerrain(normalize(gxM.xyz), pw, n, lights, hemi) * gxF.yzw;
            }

            if (wmoSpecular && (features & GX_LIGHT_PERPIXEL) != 0u) {
                c1.rgb = GxStockWmoSpecular(n, p);
            }
        }
    }

    float power = 0.0;

    if ((features & GX_LIGHT_SPECULAR) != 0u) {
        power = wmoSpecular ? max(GXK(13).w, 1.0) : (env && valid ? 24.0 : 0.0);
    }

    // PBR-lite: a roughness and metalness guessed per draw category (no PBR maps); misc.y - 1 is the engine's
    // category (GxLighting.h GxLightingMaterial)
    bool pbr = (features & GX_LIGHT_PBR) != 0u && valid;
    bool specOn = true;
    float damp = 1.0;
    float rough = 1.0;
    float a = 0.0;
    float f0 = 0.04;
    if (pbr) {
        // misc.y - 1: the category in bits 0-2, the composite body's section armour above (GxLighting.h)
        uint code = uint(max(regs.misc.y - 0.5, 0.0));
        uint cat = code & 7u;
        float metal = 0.0;
        bool body = cat == 6u;
        if (body) {
            uint arm = GxSkinArmor(code >> 3u, gxUv0);
            cat = arm == 2u ? 5u : (arm == 1u ? 4u : 1u);
        }
        if (family == GX_SURF_TERRAIN) {
            rough = 0.9;
        } else if (family == GX_SURF_WMO) {
            rough = wmoSpecular ? 0.5 : 0.8;
        } else if (cat == 5u || cat == 4u) {
            // ARMORMETAL: plate / mail (lightMetalPlate, lightMetalMail); the body's composite has no specular alpha
            rough = cat == 5u ? gxArmor.y : gxArmor.w; metal = cat == 5u ? gxArmor.x : gxArmor.z;
            gxMetalLum = 1.0; gxMask = body ? 0.0 : 1.0;
        } else if (cat == 3u) {
            rough = 0.7;    // cloth and leather armour, capes
        } else if (env && cat != 1u) {
            // sphere-mapped (the stock shiny layer) doodads and items: metal only on grey, bright texels under the alpha
            rough = 0.4; metal = 0.6; gxMask = 1.0; gxMetalLum = 1.0; gxMetalGrey = 1.0;
        } else if (cat == 2u) {
            // weapons and other item models: the same per-texel mask (wooden hafts, leather grips stay dull)
            rough = 0.45; metal = 0.6; gxMask = 1.0; gxMetalLum = 1.0; gxMetalGrey = 1.0;
        } else if (cat == 1u) {
            rough = 0.65;
        } else {
            rough = 0.85;
        }
        rough = clamp(rough + uintBitsToFloat(gxLightFlags.z), 0.05, 1.0);
        a = rough * rough;
        // the GGX only under lightSpecular; a rough non-metal's damped toward Lambert ((1 - rough)^2 * 2: 0.85 -> 0.045)
        specOn = (features & GX_LIGHT_SPECULAR) != 0u;
        damp = metal > 0.0 || wmoSpecular ? 1.0 : min(2.0 * (1.0 - rough) * (1.0 - rough), 1.0);
        f0 = mix(0.04, 0.5, metal);
        gxF0 = f0;
        gxMetal = metal;
        gxMask = wmoSpecular ? 1.0 : gxMask;   // the stock MapObjSpecular masks its highlight by the texture alpha
        power = 0.0;
    }

    vec3 spec;
    vec3 dyn = GxDynamic(p, n, power, spec, specOn ? a : 0.0, f0) * gxLightParams.y;
    float kd = 1.0;
    vec3 pbrSpec = vec3(0.0);

    if (pbr) {
        vec3 v = normalize(-p);
        vec3 sunCol = family == GX_SURF_M2 ? GXK(10).rgb : (family == GX_SURF_WMO ? GXK(11).rgb : GXK(26).rgb);
        vec3 amb = family == GX_SURF_M2 ? GXK(11).rgb : (family == GX_SURF_WMO ? GXK(10).rgb : GXK(25).rgb);
        vec3 l = family == GX_SURF_TERRAIN ? normalize(GXK(24).xyz) : -GXK(12).xyz;
        // the sky's reflection: Karis' analytic split-sum, the sky tint above and the ground's below the reflected ray
        float nv = max(dot(n, v), 1e-4);
        vec4 rr = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
        vec2 ab = vec2(-1.04, 1.04) * (min(rr.x * rr.x, exp2(-9.28 * nv)) * rr.x + rr.y) + rr.zw;
        float specAlbedo = f0 * ab.x + ab.y;
        float up = clamp(dot(reflect(-v, n), gxUpView.xyz) * 0.5 + 0.5, 0.0, 1.0);
        float k = family == GX_SURF_M2 ? gxLightParams.z : gxLightParams.w;
        gxSpecSun = specOn ? GxDec(sunCol) * (GxGgx(n, v, l, a, f0) * k * damp) : vec3(0.0);
        gxSpecAmb = GxDec(amb) * mix(gxGroundTint.rgb, gxSkyTint.rgb, up) * (specAlbedo * k);
        pbrSpec = spec * (gxLightParams.y * k * damp);
        // energy conservation: what the specular reflects the diffuse gives up
        kd = clamp(1.0 - specAlbedo * k, 0.0, 1.0);
        gxAmbCap = (1.0 - kd) / max(kd, 0.05);
    }

    // lightingDebug 3: the dynamic light alone
    c.rgb = gxLightFlags.w == 3u ? GxEnc(dyn) : GxEnc((GxDec(c.rgb) + GxDec(mat) * dyn) * kd);

    if (pbr) {
        gxSpecular = pbrSpec;
        if (family == GX_SURF_TERRAIN) {
            // the stock terrain shader masks COLOR1 by the layer's alpha, shadows it (shadow map and baked MCSH) and fogs it
            c1.rgb = GxEnc(gxSpecSun);
            gxSpecSun = vec3(0.0);
        } else if (wmoSpecular) {
            c1.rgb = vec3(0.0);         // the highlight goes through gxSpecSun (shadowed) instead
        }
    } else if (power > 0.0) {
        if (wmoSpecular) {
            c1.rgb = GxEnc(GxDec(c1.rgb) + spec * (gxLightParams.y * gxLightParams.w));
        } else {
            vec3 h = normalize(normalize(-p) - GXK(12).xyz);
            gxSpecSun = GxDec(GXK(10).rgb) * (pow(clamp(dot(n, h), 0.0, 1.0), power) * gxLightParams.z);
            gxSpecular = spec * (gxLightParams.y * gxLightParams.z);
        }
    }

    return c;
}
'''

SPEC_ADD = ('    oC0.rgb = any(greaterThan(gxSpecular + gxSpecSun + gxSpecAmb, vec3(0.0))) ? GxEnc(GxDec(oC0.rgb) + GxSpecFinal(oC0))'
            ' : oC0.rgb;')


def surface_outputs(em):
    """His Writer::SurfaceOutputs: the lines a lit vertex module runs after its body (before the y flip), or
    None when the record is not one this knows (or its names' families disagree). em: the Emitter after its
    body pass."""
    out = [_surface(em, f) for f in sorted(em.families)] or [None]
    return out[0] if all(o == out[0] for o in out) else None


def _surface(em, fam):
    if not em.vs or fam in (FAMILY_NONE, FAMILY_LITPIXEL):
        return None
    reg = {}
    for nm, sem in em.inputs.items():
        reg.setdefault(sem, nm)
    pos, weights, indices = reg.get('position0'), reg.get('blendweight0'), reg.get('blendindices0')
    normal, color = reg.get('normal0'), reg.get('color0')
    outs = {}
    for nm, sems in em.out_sems.items():
        for s in sems:
            outs.setdefault(s, nm)
    opos, oc0 = outs.get('position0'), outs.get('color0', 'vec4(0.0)')

    def reads(first, last):
        return all(c in em.creads for c in range(first, last + 1))

    if pos is None or opos is None:
        return None
    clip_ok = 'all(lessThanEqual(abs(gxClip - %s), vec4(0.001 * abs(%s.w) + 0.0001))) ? 1.0 : 0.0' % (opos, opos)
    s = ['    // the surface, for the pixel module\'s lighting (bls/lighting.py)',
         '    gxN = vec4(0.0);', '    gxP = vec4(0.0);', '    gxM = vec4(0.0);', '    gxF = vec4(0.0);',
         '    if (gxLightFlags.x != 0u) {']
    A = s.append

    if fam == FAMILY_TERRAIN:
        # Terrain: C0-C3 the chunk's world matrix, C4-C7 view and projection, C24-C26 the sun
        if normal is None or not reads(0, 7) or not reads(24, 26):
            return None
        A('        vec4 gxW = regs.v[0] * {0}.x + regs.v[1] * {0}.y + regs.v[2] * {0}.z + regs.v[3];'.format(pos))
        A('        vec4 gxClip = gxW.x * regs.v[4] + gxW.y * regs.v[5] + gxW.z * regs.v[6] + gxW.w * regs.v[7];')
        A('        vec3 gxNw = regs.v[0].xyz * {0}.x + regs.v[1].xyz * {0}.y + regs.v[2].xyz * {0}.z;'.format(normal))
        lights = 0
        while lights < 3 and reads(28 + lights * 3, 30 + lights * 3):
            lights += 1
        A('        vec3 gxL = GxStockTerrain(gxNw, gxW.xyz, gxNw, %du, false);' % lights)
        if color is not None:
            # the vertex-coloured chunks (MCCV): the lighting times the colour
            A('        vec3 gxK = %s.rgb;' % color)
        else:
            # the layers that blend mod2x get half the lighting
            A('        vec3 gxK = vec3(dot(abs({0}.rgb - gxL * 0.5), vec3(1.0)) < dot(abs({0}.rgb - gxL), vec3(1.0)) ? 0.5 : 1.0);'.format(oc0))
        A('        float gxOk = all(lessThan(abs(%s.rgb - gxL * gxK), vec3(0.004))) ? 1.0 : 0.0;' % oc0)
        # C0-C3 is world * view (CWorldScene::RenderChunks): gxW and gxNw are view space already, not world
        A('        gxP = vec4(gxW.xyz, %s);' % clip_ok)
        A('        gxN = vec4(normalize(gxNw), gxOk);')
        A('        gxM = vec4(gxNw, 0.0);')
        A('        gxF = vec4(%d.0, gxK);' % (3 | 64 | (lights << 3)))
        A('    }')
        return s

    # M2 and WMO: C31-C33 the world-view matrix (a bone's for a skinned M2: three registers a bone, from C31),
    # C2-C5 the projection
    skinned = em.uses_a0 and indices is not None
    if not reads(2, 5) or (not skinned and not reads(31, 33)):
        return None
    wmo = fam in (FAMILY_WMO, FAMILY_WMO_SPEC, FAMILY_WMO_COMP, FAMILY_WMO_COMP_SPEC)
    composite = fam in (FAMILY_WMO_COMP, FAMILY_WMO_COMP_SPEC)
    lit = normal is not None and reads(10, 12) and (not composite or (color is not None and reads(28, 29)))
    # an M2 without lighting is unlit on purpose (glows, effects): not touched
    if not wmo and not lit:
        return None

    if skinned and weights is not None:
        A('        ivec4 gxI = ivec4(round(%s * 3.0));' % indices)
        for row in range(3):
            A('        vec4 gxR{r} = regs.v[{c} + gxI.x] * {w}.x + regs.v[{c} + gxI.y] * {w}.y + regs.v[{c} + gxI.z] * {w}.z'
              ' + regs.v[{c} + gxI.w] * {w}.w;'.format(r=row, c=31 + row, w=weights))
    elif skinned:
        A('        int gxI = int(round(%s.x * 3.0));' % indices)
        A('        vec4 gxR0 = regs.v[31 + gxI];')
        A('        vec4 gxR1 = regs.v[32 + gxI];')
        A('        vec4 gxR2 = regs.v[33 + gxI];')
    else:
        A('        vec4 gxR0 = regs.v[31];')
        A('        vec4 gxR1 = regs.v[32];')
        A('        vec4 gxR2 = regs.v[33];')
    A('        vec3 gxPos = vec3(dot(gxR0, {0}), dot(gxR1, {0}), dot(gxR2, {0}));'.format(pos))
    A('        vec4 gxH = vec4(gxPos, 1.0);')
    A('        vec4 gxClip = vec4(dot(regs.v[2], gxH), dot(regs.v[3], gxH), dot(regs.v[4], gxH), dot(regs.v[5], gxH));')
    A('        gxP = vec4(gxPos, %s);' % clip_ok)

    flags = 2 if wmo else 1
    if lit:
        flags |= 64
        A('        vec3 gxNv = normalize(vec3(dot(gxR0.xyz, {0}.xyz), dot(gxR1.xyz, {0}.xyz), dot(gxR2.xyz, {0}.xyz)));'.format(normal))
        if color is not None:
            A('        vec4 gxMat = %s;' % color)
        else:
            A('        vec4 gxMat = %s;' % ('vec4(1.0)' if wmo else 'regs.v[28]'))
        lights = 0
        while lights < 4 and (21 + lights) in em.creads:
            lights += 1
        flags |= lights << 3
        A('        vec3 gxL = %s(gxNv, gxPos, %du, false);' % ('GxStockWmo' if wmo else 'GxStockM2', lights))
        # the colour is the stock lighting's when the formula reproduces it; otherwise the surface is not relit
        if composite:
            flags |= 512
            A('        float gxOk = all(lessThan(abs(clamp(regs.v[28].rgb * gxL + gxMat.rgb + regs.v[29].rgb, 0.0, 1.0)'
              ' - %s.rgb), vec3(0.004))) ? 1.0 : 0.0;' % oc0)
        else:
            A('        float gxOk = all(lessThan(abs(clamp(gxMat.rgb * gxL + regs.v[29].rgb, 0.0, 1.0) - %s.rgb),'
              ' vec3(0.004))) ? 1.0 : 0.0;' % oc0)
        A('        gxN = vec4(gxNv, gxOk);')
        A('        gxM = vec4(gxMat.rgb, 0.0);')
    if fam in (FAMILY_WMO_SPEC, FAMILY_WMO_COMP_SPEC):
        flags |= 128
    elif fam == FAMILY_M2ENV:
        flags |= 256
    A('        gxF = vec4(%d.0, 1.0, 0.0, 0.0);' % flags)
    A('    }')
    return s


# WMONORMALS: the stage a MapObj pixel module reads its material's normal map from (GxLighting.h kWmoNormalStage)
WMO_NORMAL_SLOT = 10


def pixel_colors(em):
    """His Writer::LitPixel: the COLOR0 / COLOR1 input registers of a lit pixel record (rgb read), the TEXCOORD0
    input of a MapObj one (its normal map's uv, WMONORMALS; None otherwise) and the TEXCOORD0 input (ARMORMETAL: the
    composite skin's uv; None without), or None."""
    if em.vs or em.families != {FAMILY_LITPIXEL}:
        return None
    c = {}
    for nm, sem in em.inputs.items():
        if sem in ('color0', 'color1') and (em.in_masks.get(nm, 0xF) & 7) == 7:
            c.setdefault(sem, nm)
        if sem == 'texcoord0':
            c.setdefault(sem, nm)
    names = getattr(em.rec, 'names', None) or []
    wmo = bool(names) and all(n.lower().startswith('mapobj') for n in names) and WMO_NORMAL_SLOT not in em.samplers
    return (c['color0'], c.get('color1'), c.get('texcoord0') if wmo else None, c.get('texcoord0')) if 'color0' in c else None


# ARMORMETAL (shadow_capture): the stock body's shadow factor multiplying the lit COLOR0 (M2 and WMO: `rA.xyz = rS.www *
# v0.xyz`), the mad that made it from the shadow map's visibility (`rS.w = rV.x * c14.x + c14.y`), and the first
# s0 sample (the albedo)
import re as _re
_GLSL = dict(anchor=r'(r\d+)\.([xyzw])\2\2 \* {c0}\.xyz\b', mad=r'^\s*{r}\.{c} = (r\d+\.[xyzw]) \* \S+ \+ \S+;',
             tex=r'^\s*(r\d+) = texture\(s0, ', set='    gx{0} = {1};')


def shadow_capture(body, c0, d=_GLSL):
    """The body with gxShadow (the sun's visibility, 1 lit) set before the stock shader shadows the lit colour, and
    gxTex0 after its first s0 sample. Terrain shadows its texel instead and needs neither (its COLOR1 is shadowed)."""
    body = list(body)
    for i, ln in enumerate(body):
        m = _re.match(d['tex'], ln)
        if m:
            body.insert(i + 1, d['set'].format('Tex0', m.group(1)))
            break
    pa = _re.compile(d['anchor'].format(c0=_re.escape(c0)))
    for i, ln in enumerate(body):
        m = pa.search(ln)
        if not m:
            continue
        r, c = m.group(1), m.group(2)
        for j in range(i - 1, max(i - 4, -1), -1):
            mm = _re.match(d['mad'].format(r=r, c=c), body[j])
            if mm:
                body.insert(j, d['set'].format('Shadow', mm.group(1)))
                return body
        body.insert(i, d['set'].format('Shadow', 'clamp(%s.%s, 0.0, 1.0)' % (r, c)))
        return body
    return body


def pixel_splice(c0, c1, uv=None, uv0=None):
    """The lines after the pixel module's prologue: COLOR0 (and COLOR1) lit per pixel; the face normal, and a
    MapObj module's normal-map frame (uv: its TEXCOORD0 input), are taken here, outside any branch."""
    s = ['    // the colour lit per pixel (bls/lighting.py)',
         '    vec3 gxFlatN = cross(dFdy(gxP.xyz), dFdx(gxP.xyz));',
         '    gxFlatN = dot(gxFlatN, gxFlatN) > 1e-24 ? normalize(gxFlatN) : vec3(0.0, 0.0, -1.0);',
         '    gxFlatN = dot(gxFlatN, gxP.xyz) > 0.0 ? -gxFlatN : gxFlatN;',
         '    vec4 gxNMap = vec4(0.0);']
    if uv:
        s += ['    // WMONORMALS: the normal map (wmoNormalStrength in gxGroundTint.w, 0 = off)',
              '    vec3 gxDp1 = dFdx(gxP.xyz);',
              '    vec3 gxDp2 = dFdy(gxP.xyz);',
              '    vec2 gxDu1 = dFdx(%s.xy);' % uv,
              '    vec2 gxDu2 = dFdy(%s.xy);' % uv,
              '    float gxNStrength = gxGroundTint.w;']
    s += ['    if (gxLightFlags.x != 0u && regs.misc.y > 0.5 && gxF.x > 0.5 && gxP.w > 0.999) {']
    if uv:
        s += ['        if (gxNStrength > 0.0) {',
              '            gxNMap = GxWmoNormal(gxN.xyz, gxDp1, gxDp2, gxDu1, gxDu2, textureGrad(s%d, %s.xy, gxDu1, gxDu2), gxNStrength);'
              % (WMO_NORMAL_SLOT, uv),
              '        }']
    s += ['        vec4 gxC1 = %s;' % (c1 or 'vec4(0.0)'),
          '        %s = GxLightPixel(%s, gxC1, gxFlatN, gxNMap);' % (c0, c0)]
    if uv0:
        s.insert(-1, '        gxUv0 = %s.xy;' % uv0)
    s += (['        %s = gxC1;' % c1] if c1 else []) + ['    }', '']
    return s
