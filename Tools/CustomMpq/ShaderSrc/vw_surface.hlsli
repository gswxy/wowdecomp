// VFogWater: coa-vfog shaders/vw_surface.hlsli (@ 4e31ddf, GPLv3) with the moments rebuilt from the CPU wave textures
// (SampleMoments) and R32F depth copies.
static const float kPi = 3.14159265;
static const float kShallowTileAmplitude = 800;
static const float kMaxTanhArgument = 10;
static const float kAdtGridOrigin = 17066.666;
static const float kAdtTilesPerYard = 0.001875;
static const float kSlopeToAdtUv = 0.001;
static const float kMinFoamFade = 1e-4;
static const float kMinRayRise = 1e-4;
static const float kRippleSlopeScale = 3;
static const float kRippleFullDetailTexels = 2;
static const float kRippleDetailFadeTexels = 2;

struct WaterPixel
{
    float2 uv;
    float waterZ;
    float sceneZ;
    float3 position;
    float3 toCamera;
    float cameraDistance;
    float columnDepth;
    float2 footprintX;
    float2 footprintY;
};

struct WaveState
{
    float4 moments;
    float2 foam;
};

struct RippleSample
{
    float height;
    float2 slope;
    float coverage;
};

float2 CopyUv(float2 pixel)
{
    return (pixel - ViewportOrigin()) / ViewportSize();
}

// R32F copies (coa's r32f path; no packed rgba8 fallback). sWaterDepth is signed: abs() is the view Z.
float CopiedViewDepth(sampler2D copy, float2 uv)
{
    return abs(tex2Dlod(copy, float4(uv, 0, 0)).r);
}

float3 GammaToLinear(float3 colour)
{
    return pow(max(colour, 0), 2.2);
}

float3 LinearToGamma(float3 colour)
{
    return pow(saturate(colour), 1 / 2.2);
}

float2 PlaneFootprint(float3 ray, float3 rayStep, float viewZ)
{
    float rise = ray.z < 0 ? min(ray.z, -kMinRayRise) : max(ray.z, kMinRayRise);
    return viewZ * (rayStep.xy - ray.xy * rayStep.z / rise);
}

WaterPixel ReconstructWaterPixel(float2 pixel)
{
    WaterPixel w;
    w.uv = CopyUv(pixel);
    w.waterZ = CopiedViewDepth(sWaterDepth, w.uv);
    w.sceneZ = max(CopiedViewDepth(sSceneDepth, w.uv), w.waterZ);
    float3 worldRay = ViewToWorldDirection(ViewRayAtUnitDepth(pixel));
    w.position = CameraPositionWorld() + worldRay * w.waterZ;
    w.toCamera = normalize(-worldRay);
    w.cameraDistance = w.waterZ * length(worldRay);
    w.columnDepth = max(0, (w.sceneZ - w.waterZ) * -worldRay.z);
    float3 rayStepX = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(1, 0))) - worldRay;
    float3 rayStepY = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(0, 1))) - worldRay;
    w.footprintX = PlaneFootprint(worldRay, rayStepX, w.waterZ);
    w.footprintY = PlaneFootprint(worldRay, rayStepY, w.waterZ);
    return w;
}

float4 SampleTile(sampler2D tile, WaterPixel w, float inverseSize)
{
    return tex2Dgrad(tile, w.position.xy * inverseSize, w.footprintX * inverseSize, w.footprintY * inverseSize);
}

// coa's float4 moments (slope.xy, slope^2, wide slope^2) from the two CPU textures: slope from the Gr1616F tile,
// slope^2 = variance + slope^2 of the filtered mean (the variance itself is stored, so the mean's own square never
// meets an 8-bit copy of it), wide slope^2 decoded from its sqrt. kVarianceRange / kWideRange match
// VolumetricFogWaterWaves.cpp.
static const float kVarianceRange = 0.25;
static const float kWideRange = 1;

float4 SampleMoments(sampler2D slopes, sampler2D state, WaterPixel w, float inverseSize)
{
    float2 slope = SampleTile(slopes, w, inverseSize).xy;
    float2 spread = SampleTile(state, w, inverseSize).xy;
    spread *= spread;
    return float4(slope, spread.x * kVarianceRange + dot(slope, slope), spread.y * kWideRange);
}

float2 SampleFoamState(sampler2D state, WaterPixel w, float inverseSize)
{
    return tex2Dlod(state, float4(w.position.xy * inverseSize, 0, 0)).zw;
}

float Tanh(float x)
{
    float e = exp(2 * min(x, kMaxTanhArgument));
    return (e - 1) / (e + 1);
}

float ShallowTileWeight(float inverseTileSize, float columnDepth)
{
    float shallowAmplitude = kShallowTileAmplitude * inverseTileSize * inverseTileSize;
    return saturate(lerp(shallowAmplitude, 1, Tanh(4 * kPi * columnDepth * inverseTileSize)));
}

// coa's ripples: s12 = the Gr1616F ripple map (height, previous height) around the camera target, c65-c67 its window.
float RippleHeight(float2 uv)
{
    float2 state = tex2Dlod(sRipples, float4(uv, 0, 0)).rg;
    return lerp(state.g, state.r, RippleWeight());
}

float RippleCoverage(WaterPixel w, float2 uv)
{
    float2 fromCentre = abs(uv - 0.5) * RippleExtent();
    float window = saturate((RippleFadeEnd() - max(fromCentre.x, fromCentre.y)) * RippleInverseFadeWidth());
    float footprintTexels = max(length(w.footprintX), length(w.footprintY)) / RippleTexelYards();
    return window * saturate(1 - (footprintTexels - kRippleFullDetailTexels) / kRippleDetailFadeTexels);
}

// wxl-water RippleSlopeW: the wide map's height between its last two steps, finite differences over its own texel
// (0.5 yd), nothing outside the window. Not bent by a normal map (Cata's nn): the waves here are the moments.
float RipHW(float2 t) { float4 h = tex2Dlod(sRippleWide, float4(t, 0, 0)); return lerp(h.g, h.r, cRippleTexW.z); }
float2 RippleSlopeW(float2 world)
{
    float2 uv = (world - cRippleWinW.xy) * cRippleWinW.zw - 1e-6;
    float  h0 = RipHW(uv);
    float  dx = RipHW(uv + float2(cRippleTexW.x, 0.0)) - h0, dy = RipHW(uv + float2(0.0, cRippleTexW.y)) - h0;
    float2 sl = float2(dx, dy) / (sqrt(1.0 + dx * dx) * sqrt(1.0 + dy * dy));
    return (max(abs(2.0 * uv.x - 1.0), abs(2.0 * uv.y - 1.0)) >= 1.0 - cRippleTexW.x) ? float2(0.0, 0.0) : sl;
}

RippleSample SampleRipples(WaterPixel w)
{
    RippleSample ripples = (RippleSample)0;
    float2 uv = (w.position.xy - RippleOrigin()) * RippleInverseExtent();
    ripples.coverage = RippleGain() > 0 ? RippleCoverage(w, uv) : 0;
    [branch] if (ripples.coverage > 0)
    {
        float step = RippleTexelUv();
        ripples.height = RippleHeight(uv);
        float dx = RippleHeight(uv + float2(step, 0)) - ripples.height;
        float dy = RippleHeight(uv + float2(0, step)) - ripples.height;
        float2 gradient = float2(dx, dy) * rsqrt((1 + dx * dx) * (1 + dy * dy));
        ripples.slope = gradient * kRippleSlopeScale * RippleGain() * ripples.coverage;
    }
    // The ships' wide level (wxl-water RippleSlopeW, its own map and window), at the same gain as the close field.
    [branch] if (cRippleTexW.w > 0)
    {
        ripples.slope += RippleSlopeW(w.position.xy) * kRippleSlopeScale * cRippleTexW.w;
    }
    return ripples;
}

WaveState SampleWaves(WaterPixel w, float2 rippleSlope)
{
    float4 inverseSize = cInverseTileSizes;
    float4 weight = float4(ShallowTileWeight(inverseSize.x, w.columnDepth),
                           ShallowTileWeight(inverseSize.y, w.columnDepth),
                           ShallowTileWeight(inverseSize.z, w.columnDepth),
                           ShallowTileWeight(inverseSize.w, w.columnDepth));
    WaveState waves;
    waves.moments = weight.x * SampleMoments(sSurface0, sFoamState0, w, inverseSize.x) +
                    weight.y * SampleMoments(sSurface1, sFoamState1, w, inverseSize.y) +
                    weight.z * SampleMoments(sSurface2, sFoamState2, w, inverseSize.z) +
                    weight.w * SampleMoments(sSurface3, sFoamState3, w, inverseSize.w);
    waves.moments.xy += rippleSlope;
    waves.foam = weight.x * SampleFoamState(sFoamState0, w, inverseSize.x) +
                 weight.y * SampleFoamState(sFoamState1, w, inverseSize.y) +
                 weight.z * SampleFoamState(sFoamState2, w, inverseSize.z) +
                 weight.w * SampleFoamState(sFoamState3, w, inverseSize.w);
    return waves;
}

float SlopeVariance(float4 moments)
{
    float2 meanSlope = moments.xy * InverseTileCount();
    return 0.5 * max(0, moments.z * InverseTileCount() - dot(meanSlope, meanSlope));
}

float CrestTilt(float4 moments)
{
    float coarseSlope = moments.w * InverseTileCount();
    return sqrt(coarseSlope / (coarseSlope + 1));
}

float2 AdtUv(float3 position, float2 slope)
{
    return (kAdtGridOrigin - position.yx) * kAdtTilesPerYard + slope * kSlopeToAdtUv;
}

float4 TintedFoam(int slot, float maskCoverage)
{
    float coverage = maskCoverage * MaskPresent(slot);
    return float4(lerp(MaskTintLow(slot), MaskTintHigh(slot), coverage), coverage);
}

// Shore and depth foam (2026-10-25 foam2): LuxVelum's shore mask (the wave masks' alpha) at each layer's scale and
// scroll, a second tap at 0.73x, axes swapped and counter-scrolled (cDepthFoamScroll.zw) so it churns and the mask's
// holes fill, eroded by the fade: at the edge most of the mask shows, further out only its peaks (lace, not a sheet).
float4 FoamLayer(int slot, float2 uv, float2 dx, float2 dy, float fade)
{
    float a = tex2Dgrad(sWaveFoamMasks, uv, dx, dy).a;
    float b = tex2Dgrad(sWaveFoamMasks, uv.yx * 0.73 + cDepthFoamScroll.zw, dx.yx * 0.73, dy.yx * 0.73).a;
    float m = max(a, b);
    float4 foam = TintedFoam(slot, m);
    foam.rgb *= fade;   // LuxVelum: the tint scaled by the layer weight
    foam.a = 0.4 * smoothstep(1 - fade, 1.35 - fade, m) * MaskPresent(slot);   // 0.4: half of foam's 0.8
    return foam;
}

// The water column at this point in world space: the water plane's z minus the land's (FoamLand, bilinear by hand
// over point taps). Nothing on screen goes into it, so the foam is the same from every camera. No land: 1e4 (none).
float FoamColumnDepth(float3 position)
{
    [branch] if (cFoamLand.w <= 0)
        return 1e4;
    float2 t = cFoamLand.xy - cFoamLand.z * position.yx;   // texel k holds grid point k
    float2 f = frac(t);
    float2 uv = (floor(t) + 0.5) * cFoamLand.w;
    float h00 = tex2Dlod(sFoamLand, float4(uv, 0, 0)).r;
    float h01 = tex2Dlod(sFoamLand, float4(uv + float2(cFoamLand.w, 0), 0, 0)).r;
    float h10 = tex2Dlod(sFoamLand, float4(uv + float2(0, cFoamLand.w), 0, 0)).r;
    float h11 = tex2Dlod(sFoamLand, float4(uv + cFoamLand.ww, 0, 0)).r;
    float land = lerp(lerp(h00, h01, f.x), lerp(h10, h11, f.x), f.y);
    bool inside = all(abs(uv - 0.5) < 0.5 - cFoamLand.w);
    return inside && min(min(h00, h01), min(h10, h11)) > -20000 ? max(position.z - land, 0) : 1e4;
}

float FoamFade(float distance, float fadeDistance)
{
    float t = saturate(1 - distance / max(fadeDistance, kMinFoamFade));
    float eased = t * t * (3 - 2 * t);
    eased *= eased;
    return eased * eased;
}

float4 WaveFoam(float2 adtUv, float2 dx, float2 dy, float f)
{
    float3 scale = cWaveFoamScaling.xyz;
    float4 high = TintedFoam(kHighFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.x + cFoamScroll.xy, dx * scale.x,
                                                      dy * scale.x).r);
    float4 mid = TintedFoam(kMidFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.y + cFoamScroll.xy, dx * scale.y,
                                                    dy * scale.y).g);
    float4 low = TintedFoam(kLowFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.z + cFoamScroll.xy, dx * scale.z,
                                                    dy * scale.z).b);
    float rootF = sqrt(f);
    return saturate(low * rootF + mid * f * rootF + high * pow(f, 4.5)) * WaveFoamIntensity();
}

float4 FoamAlbedo(WaterPixel w, WaveState waves)
{
    float2 adtUv = AdtUv(w.position, waves.moments.xy);
    float2 dx = -w.footprintX.yx * kAdtTilesPerYard;
    float2 dy = -w.footprintY.yx * kAdtTilesPerYard;
    float f = max(waves.foam.x, 0);
    // The band washes in and out across the shore (cWaveFoamScaling.w = the phase, one wave every 6.5 s): its edge runs
    // up the beach in the first third, then drains back. The fade distances are world yards of water column.
    float s = cWaveFoamScaling.w;
    float wash = s < 0.33 ? smoothstep(0, 0.33, s) : 1 - smoothstep(0.33, 1, s);
    float reach = 0.5 + 0.7 * wash;
    float column = FoamColumnDepth(w.position);
    float shoreFade = FoamFade(column * ShoreDistancePerDepth(), cShoreFoam.z * reach);
    float depthFade = FoamFade(column, cDepthFadeFoam.z * reach);
    float4 foam = 0;
    [branch] if (f * WaveFoamIntensity() > 0)
        foam = WaveFoam(adtUv, dx, dy, f);
    [branch] if (cShoreFoam.x * shoreFade > 0)
        foam += cShoreFoam.x * FoamLayer(kShoreFoamSlot, adtUv * cShoreFoam.y + cFoamScroll.zw,
                                         dx * cShoreFoam.y, dy * cShoreFoam.y, shoreFade);
    [branch] if (cDepthFadeFoam.x * depthFade > 0)
        foam += cDepthFadeFoam.x * FoamLayer(kDepthFoamSlot, adtUv * cDepthFadeFoam.y + cDepthFoamScroll.xy,
                                             dx * cDepthFadeFoam.y, dy * cDepthFadeFoam.y, depthFade);
    return saturate(foam);
}
