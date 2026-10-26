// VFOG WP1: CPU fog model (port of coa-vfog src/fog_model.cpp @ 4e31ddf, plus wxl-vfog's StartOffset)
// and the CPU ray integrator shared by the stock-fog fits (coa-vfog src/transparent_fog.cpp).
#include "WoW/Source/WorldClient/VolumetricFogModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace VolumetricFog {

namespace {

// ---- fog_model.cpp:9-50 ----
constexpr float kHazeDensity = 0.00035f;
constexpr float kHazeG = 0.75f;
constexpr float kHazeIsotropic = 0.3f;
constexpr float kHazeFalloff = 1.0f / 220.0f;
constexpr float kHazeSun = 1.3f;
constexpr float kHazeAmbient = 0.8f;

constexpr float kGroundStart = 12.0f;
constexpr float kGroundOpticalDepth = 1.1f;
constexpr float kGroundG = 0.4f;
constexpr float kGroundIsotropic = 0.5f;
constexpr float kGroundFalloff = 1.0f / 18.0f;
constexpr float kGroundSun = 0.8f;
constexpr float kGroundAmbient = 0.75f;

constexpr float kFarOpticalDepth = 3.0f;
// vfogaudit: with far land drawing (FarLand::FogCap past farclip) the distance layer ends at e^-2.3 ~ 10 % of the land
// showing through, so its silhouette and ridge lines stay readable against the sky (3.0 left ~5 %: a flat wall).
constexpr float kFarLandOpticalDepth = 2.3f;
constexpr float kFarRamp = 60.0f;
constexpr float kFarStartFraction = 0.25f;
constexpr float kFarG = 0.2f;
constexpr float kFarIsotropic = 0.7f;
constexpr float kFarSun = 0.45f;
constexpr float kFarAmbient = 0.6f;
constexpr float kFarSkyFalloff = 10.0f;
constexpr float kFarExponent = 2.0f;

constexpr float kClassicUnits = 0.01f;
constexpr float kFogRangeMargin = 300.0f;
constexpr uint32_t kFlagShadowed = 0x1;
constexpr uint32_t kFlagRelativeHeights = 0x2;

constexpr float kDerivedLayerLimit = 1500.0f;

constexpr float kNoiseCurveShape = (kNoiseCurveContrast - 1.0f) / (2.0f - kNoiseCurveContrast);
constexpr float kFourPi = 12.5663706f;
constexpr float kMaxEnergyNormalisedPhaseG = 0.95f;
constexpr int kEnergyNormalisedClassicPhase = 1;

constexpr float kLightSettingHalfWidth = 0.02f;
constexpr float kMinClassicDirectLuminance = 1.0e-3f;
constexpr float kLuminanceWeights[3] = { 0.2126f, 0.7152f, 0.0722f };
constexpr float kReferenceFarTarget = 200.0f;
constexpr float kNoLimit = 1.0e9f;


void Scale(const float* rgb, float s, float* out) {
    out[0] = rgb[0] * s;
    out[1] = rgb[1] * s;
    out[2] = rgb[2] * s;
}

bool ScattersInLinearLight(const Config& cfg) {
    return cfg.colorSpace == 1;
}

// Gamma decode into linear light (ColorSpace = 1).
void Encode(float* rgb, bool linear) {
    if (linear) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] = std::pow(std::max(rgb[i], 0.0f), 2.2f);
        }
    }
}

float Luminance(const float* rgb) {
    return rgb[0] * kLuminanceWeights[0] + rgb[1] * kLuminanceWeights[1] + rgb[2] * kLuminanceWeights[2];
}

// fog_model.cpp:82-90
float ReferenceZ(const FrameInputs& in) {
    float dx = in.camTarget[0] - in.camPos[0];
    float dy = in.camTarget[1] - in.camPos[1];
    float dz = in.camTarget[2] - in.camPos[2];
    if (dx * dx + dy * dy + dz * dz > kReferenceFarTarget * kReferenceFarTarget) {
        return in.camPos[2] - 2.0f;
    }
    return std::min(in.camPos[2], in.camTarget[2]) - 1.0f;
}

void Unshadowed(FogLayer& l) {
    std::memcpy(l.shadowEmissive, l.emissive, sizeof(l.emissive));
    l.shadowDensity = 1.0f;
    l.shadowed = 0.0f;
}

void Unbounded(FogLayer& l) {
    l.skyFalloff = 0.0f;
    l.endDistance = kNoLimit;
    l.densityVariation = 1.0f;
}

// fog_model.cpp:106-141
void DerivedLayers(const Config& cfg, const FogParams& p, float sunScatter, float fogDistance, const float* fogColor,
                   FogLayer* out) {
    float foggyZone = std::clamp(700.0f / fogDistance, 0.6f, 2.5f);

    FogLayer& haze = out[0];
    haze.start = 0.0f;
    haze.density = kHazeDensity * cfg.haze * cfg.density * foggyZone;
    haze.g = kHazeG;
    haze.isotropic = kHazeIsotropic;
    Scale(fogColor, kHazeAmbient * cfg.ambient, haze.emissive);
    Scale(p.lightColor, kHazeSun * sunScatter, haze.diffuse);
    haze.exponent = 1.0f;
    haze.upperHeight = p.referenceZ;
    haze.upperFalloff = kHazeFalloff;
    Unshadowed(haze);
    Unbounded(haze);
    haze.endDistance = kDerivedLayerLimit;

    FogLayer& ground = out[1];
    ground.start = kGroundStart;
    ground.density = kGroundOpticalDepth * cfg.groundFog * cfg.density / std::max(fogDistance, 250.0f);
    ground.g = kGroundG;
    ground.isotropic = kGroundIsotropic;
    Scale(fogColor, kGroundAmbient * cfg.ambient, ground.emissive);
    Scale(p.lightColor, kGroundSun * sunScatter, ground.diffuse);
    ground.exponent = 1.0f;
    ground.upperHeight = p.referenceZ;
    ground.upperFalloff = kGroundFalloff;
    Unshadowed(ground);
    Unbounded(ground);
    ground.endDistance = kDerivedLayerLimit;

    out[2] = FogLayer{};
    Unbounded(out[2]);
}

// fog_model.cpp:143-160
LayerNoise AuthoredLayerNoise(const AuthoredNoise& authored, const Config& cfg, bool linear) {
    LayerNoise noise = {};
    const float octaveShares = authored.octaveShare[0] + authored.octaveShare[1];
    if (!cfg.classicNoise || authored.presence <= 0.0f || octaveShares <= 0.0f) {
        return noise;
    }
    noise.alpha = std::min(authored.presence, 1.0f);
    for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
        noise.octaveWeight[octave] = authored.octaveShare[octave] / octaveShares;
        noise.inverseTileYards[octave] = authored.tileYards[octave] > 0.0f ? 1.0f / authored.tileYards[octave] : 0.0f;
        std::memcpy(noise.velocity[octave], authored.velocity[octave], sizeof(noise.velocity[octave]));
    }
    Scale(authored.fade, cfg.ambient, noise.fade);
    Encode(noise.fade, linear);
    return noise;
}

// fog_model.cpp:162-199
void AuthoredLayers(const AuthoredFog& fog, const Config& cfg, const FogParams& p, float sunScatter, FogLayer* out,
                    LayerNoise* noise) {
    for (int i = 0; i < kSceneLayers; ++i) {
        FogLayer& l = out[i];
        l = FogLayer{};
        noise[i] = LayerNoise{};
        Unbounded(l);
        if (i >= fog.layerCount) {
            continue;
        }
        const AuthoredLayer& a = fog.layers[i];
        noise[i] = AuthoredLayerNoise(a.noise, cfg, p.linear);
        l.densityVariation = 1.0f - noise[i].alpha;
        const float heightBase = (a.flags & kFlagRelativeHeights) ? p.referenceZ : 0.0f;
        l.start = std::clamp(a.start, 0.0f, std::max(p.maxDistance - kFogRangeMargin, 0.0f));
        l.density = a.density * kClassicUnits * cfg.density;
        l.g = std::clamp(a.g, -0.99f, 0.99f);
        l.isotropic = 0.0f;
        Scale(a.emissive, cfg.ambient, l.emissive);
        l.strength = a.strength;
        Scale(a.diffuse, 1.0f, l.diffuse);
        l.exponent = a.exponent > 0.0f ? a.exponent : 1.0f;
        l.upperHeight = a.upperHeight + heightBase;
        l.upperFalloff = a.upperDensity > 0.0f ? a.upperDensity * kClassicUnits : 0.0f;
        l.lowerHeight = a.lowerHeight + heightBase;
        l.lowerFalloff = a.lowerDensity > 0.0f ? a.lowerDensity * kClassicUnits : 0.0f;
        Scale(a.shadowEmissive, cfg.ambient, l.shadowEmissive);
        l.shadowDensity = a.shadowMultiplier;
        l.shadowed = (a.flags & kFlagShadowed) ? 1.0f : 0.0f;
        Encode(l.emissive, p.linear);
        Encode(l.diffuse, p.linear);
        Encode(l.shadowEmissive, p.linear);
        const float phaseScale =
            cfg.classicPhase == kEnergyNormalisedClassicPhase ? EnergyNormalisedPhaseScale(l.g) : 1.0f;
        Scale(l.diffuse, a.intensity * sunScatter * phaseScale, l.diffuse);
    }
}

// fog_model.cpp:201-226
void ForeverDistanceLayer(const FrameInputs& in, const Config& cfg, const FogParams& p, FogLayer& out);

float DistanceFogOpticalDepthPerDensity(float farStart, float farLimit, float range) {
    const float spanFraction = (farLimit - farStart) / range;
    return range * (spanFraction + kFarRamp * spanFraction * spanFraction * spanFraction / 3.0f);
}

void DistanceLayer(const FrameInputs& in, const Config& cfg, const FogParams& p, float sunScatter,
                   const float* fogColor, FogLayer& out) {
    if (in.foreverFog) {
        ForeverDistanceLayer(in, cfg, p, out);
        return;
    }
    out = FogLayer{};
    float farStart = std::min(std::max(in.fogStart, 0.0f), p.farLimit * kFarStartFraction);
    float opticalDepthPerDensity = DistanceFogOpticalDepthPerDensity(farStart, p.farLimit, p.maxDistance);
    out.start = farStart;
    // coa: vfogStockFog 0 keeps the stock fog and drops the distance layer.
    const float endOpticalDepth = in.fogCap > in.farClip * 1.01f ? kFarLandOpticalDepth : kFarOpticalDepth;
    out.density = cfg.stockFog && opticalDepthPerDensity > 0.0f ? endOpticalDepth * cfg.farFog * cfg.density / opticalDepthPerDensity
                                                : 0.0f;
    out.g = kFarG;
    out.isotropic = kFarIsotropic;
    out.strength = kFarRamp;
    out.exponent = kFarExponent;
    out.skyFalloff = kFarSkyFalloff;
    out.endDistance = p.farLimit;
    // farlandv2: thinner with height above the reference (farLandHazeHeight), so low far land goes into the haze first
    // and high ridges keep a silhouette; the normalisation above stays the level ray's.
    // farLandHazeGround: BENILLA's long-range height fog lies on the regional ground, not the camera
    out.upperHeight = in.hazeReferenceValid ? in.hazeReferenceZ : p.referenceZ;
    out.upperFalloff = in.hazeFalloff;
    Scale(fogColor, kFarAmbient * cfg.ambient, out.emissive);
    Scale(p.lightColor, kFarSun * sunScatter, out.diffuse);
    Unshadowed(out);
}

// ---- foreverFog: BENILLA volumetrics/media.rs solve on the distance layer ----------------------------------------
constexpr float kForeverFarTau = 1.6f;          // FAR_TAU
constexpr float kShadedShare = 0.7f;            // SHADED_SHARE
constexpr float kReferencePhase = 0.5f;         // REFERENCE_PHASE
constexpr float kForwardG = 0.75f;              // FORWARD_G
constexpr float kBackwardG = -0.2f;             // BACKWARD_G
constexpr float kMinSpan = 8.0f;                // MIN_SPAN
constexpr float kMaxClearShare = 0.35f;         // MAX_CLEAR_SHARE
constexpr float kMaxAlbedo = 16.0f;             // MAX_ALBEDO
constexpr float kMaxFogSaturation = 0.3f;       // MAX_FOG_SATURATION
constexpr float kMaxFogLevel = 0.78f;           // MAX_FOG_LEVEL
constexpr float kStormClear = 120.0f;           // STORM_CLEAR

// media.rs subtle(): saturation <= 0.3, brightest channel <= 0.78, hue kept (gamma).
void Subtle(float* c) {
    for (int i = 0; i < 3; ++i) c[i] = std::max(c[i], 0.0f);
    const float hi = std::max(c[0], std::max(c[1], c[2]));
    if (hi <= 1e-5f) return;
    const float lo = std::min(c[0], std::min(c[1], c[2]));
    const float keep = std::min(kMaxFogSaturation * hi / std::max(hi - lo, 1e-5f), 1.0f);
    const float level = std::min(kMaxFogLevel / hi, 1.0f);
    for (int i = 0; i < 3; ++i) c[i] = (hi - (hi - c[i]) * keep) * level;
}

// look.rs noon_key_row: the key row moved noon_mix of the way to noon_key at the row's own linear luminance (hue
// only, never brightness). Gamma in and out; our decode is GxDec 2.2, not sRGB.
constexpr float kNoonKey[3] = { 1.0f, 0.8f, 0.4f };
void NoonKeyRow(const ForeverLook& look, float* row) {
    const float t = std::clamp(look.noonMix, 0.0f, 1.0f);
    if (t <= 0.0f) return;
    float lin[3], hue[3];
    for (int c = 0; c < 3; ++c) {
        lin[c] = std::pow(std::max(row[c], 0.0f), 2.2f);
        hue[c] = std::pow(kNoonKey[c], 2.2f);
    }
    const float scale = Luminance(lin) / std::max(Luminance(hue), 1e-4f);
    for (int c = 0; c < 3; ++c) row[c] += (std::pow(hue[c] * scale, 1.0f / 2.2f) - row[c]) * t;
}

// Standard Henyey-Greenstein (froxelfog.wgsl hg), for the backward lobe's side-on value.
float StandardHG(float g, float mu) {
    const float d = std::max(1.0f + g * g - 2.0f * g * mu, 1e-4f);
    return (1.0f - g * g) / (d * std::sqrt(d));
}

// The distance layer as BENILLA's open-air medium: density FAR_TAU over the zone's unclamped fog span (x fog_reach),
// e per fog_falloff above the regional ground, clear to the look's range, albedo solved per channel so ambient + key
// in-scatter at REFERENCE_PHASE lands on the (tinted, subtle) DBC fog colour; then haze_tint on the ambient term and
// glow on the key (fog_composite / froxelfog dual_phase).
void ForeverDistanceLayer(const FrameInputs& in, const Config& cfg, const FogParams& p, FogLayer& out) {
    const ForeverLook& look = in.look;
    const float interior = std::clamp(in.interiorBlend, 0.0f, 1.0f);
    const float storm = std::clamp(in.stormBlend, 0.0f, 1.0f);
    float fog[3], ambient[3], key[3];
    UnpackColor(in.fogColor, fog);
    for (int c = 0; c < 3; ++c) fog[c] *= 1.0f + (look.fogTint[c] - 1.0f) * (1.0f - interior);
    Subtle(fog);
    UnpackColor(in.ambientColor, ambient);
    Encode(fog, p.linear);
    Encode(ambient, p.linear);
    // the key's horizon / moon fade (media.rs: diffuse x key_horizon)
    Scale(p.lightColor, p.lightVisibility, key);

    const float wallEnd = in.fogEnd;
    const float openEnd = std::max(in.zoneFogDistance > 50.0f && in.zoneFogDistance < 20000.0f ? in.zoneFogDistance : 0.0f,
                                   wallEnd);
    const float openStart = wallEnd > 1e-3f ? in.fogStart / wallEnd * openEnd : 0.0f;
    const float openSpan = std::max(openEnd - std::max(openStart, 0.0f), kMinSpan);
    const float density = kForeverFarTau / (openSpan * std::max(look.fogReach, 0.05f));
    const float lookClear = look.fogClear + (std::min(look.fogClear, kStormClear) - look.fogClear) * storm;
    const float nearClear = std::max(std::min(std::max(openStart, 0.0f), kMaxClearShare * openSpan), lookClear);
    const float keyScatter = std::max(look.keyScatter, 0.0f) * (1.0f - 0.75f * storm);

    float ambientIn[3], keyIn[3];
    for (int c = 0; c < 3; ++c) {
        const float lit = ambient[c] + key[c] * keyScatter * kReferencePhase;
        const float albedo = lit > 1e-5f ? std::min(fog[c] / lit, kMaxAlbedo) : 0.0f;
        ambientIn[c] = albedo * ambient[c];
        keyIn[c] = albedo * key[c] * keyScatter;
        const float target = ambientIn[c] + keyIn[c] * kReferencePhase;
        const float floor = target * kShadedShare;
        if (ambientIn[c] < floor) {
            const float keyLeft = target - ambientIn[c];
            keyIn[c] *= (target - floor) / keyLeft;
            ambientIn[c] = floor;
        }
    }

    // Dual lobe mix(hg(-0.2), hg(0.75), forward) in our one-lobe layer: our PhaseHG is HG / ((1 + g) / (1 - g)^2)
    // (peak 1), so the forward lobe is that scale x PhaseHG(0.75) and the backward lobe is held at its side-on value
    // (isotropic). The layer is exempt from vfogShafts' phase floor (max(phase, floor) would scatter the scaled key
    // every way): a negative isotropic flags it (vf_integrate LayerRayTerms), so the sun side keeps benilla's ~20x.
    const float moon = in.lightIsMoon ? SmoothStep(-0.03f, 0.10f, in.toLight[2]) : 0.0f;
    const float forward = std::clamp(look.forward * (1.0f + (look.moonGlow - 1.0f) * moon), 0.0f, 1.0f) *
                          (1.0f - 0.5f * storm);
    const float forwardScale = (1.0f + kForwardG) / ((1.0f - kForwardG) * (1.0f - kForwardG));
    const float backward = StandardHG(kBackwardG, 0.0f);
    float scale = forward * forwardScale + (1.0f - forward) * backward;
    const float isotropic = scale > 1e-5f ? (1.0f - forward) * backward / scale : 1.0f;
    // look.rs key_hue: the glow fades to white by 1 - moon_hue while the moon keys
    const float hueFade = (1.0f - std::clamp(look.moonHue, 0.0f, 1.0f)) * moon;

    out = FogLayer{};
    out.start = 0.9f * nearClear;     // froxelfog's smoothstep(0.8 clear, clear) ease as a hard start at its mean
    out.density = cfg.stockFog ? density * cfg.farFog * cfg.density : 0.0f;
    out.g = kForwardG;
    out.isotropic = -std::max(isotropic, 1e-6f);   // < 0: no shaft floor (LayerPhase takes |isotropic|)
    out.strength = 0.0f;
    out.exponent = 1.0f;
    out.skyFalloff = 0.0f;   // benilla: no sky thinning, the height falloff alone (the sky tail, vf_integrate)
    out.endDistance = std::max(std::max(in.fogCap, in.farClip), p.farLimit);
    out.upperHeight = in.hazeReferenceValid ? in.hazeReferenceZ : p.referenceZ;
    out.upperFalloff = 1.0f / std::max(look.fogFalloff, 1.0f);
    for (int c = 0; c < 3; ++c) {
        const float glow = look.glow[c] + (1.0f - look.glow[c]) * hueFade;
        out.emissive[c] = ambientIn[c] * look.hazeTint[c];
        out.diffuse[c] = keyIn[c] * glow * scale;
    }
    Unshadowed(out);
}

float RampedPathLength(const FogLayer& l, float range) {
    const float length = std::max(range - l.start, 0.0f);
    const float u = length / range;
    return length + l.strength * range * std::pow(u, l.exponent + 1.0f) / (l.exponent + 1.0f);
}

float HeightDensityFactor(const FogLayer& l, float z) {
    return std::min(std::exp((l.upperHeight - z) * l.upperFalloff), 1.0f) *
           std::min(std::exp((z - l.lowerHeight) * l.lowerFalloff), 1.0f);
}

float HorizonShadow(const FogLayer& l, float lightAboveHorizon) {
    return l.shadowed * (1.0f - lightAboveHorizon);
}

float ShadowDensityFactor(const FogLayer& l, float lightAboveHorizon) {
    return 1.0f + (l.shadowDensity - 1.0f) * HorizonShadow(l, lightAboveHorizon);
}

float SceneLayersLevelRayOpticalDepth(const FogParams& p, float cameraZ) {
    float opticalDepth = 0.0f;
    for (int i = 0; i < kSceneLayers; ++i) {
        const FogLayer& l = p.layers[i];
        const float heightFactor = HeightDensityFactor(l, cameraZ);
        const float shadowFactor = ShadowDensityFactor(l, p.lightAboveHorizon);
        const float noiseFactor = MeanNoiseDensity(p.noise[i]);
        opticalDepth += l.density * heightFactor * shadowFactor * noiseFactor * RampedPathLength(l, p.maxDistance);
    }
    return opticalDepth;
}

// fog_model.cpp:265-271
float ClassicFogThinness(const FogParams& p, const AuthoredFog& fog, float cameraZ) {
    const float coverageEdge = 1.0f - SmoothStep(kMinimumClassicCoverage, 1.0f, fog.coverage);
    const float levelRayOpticalDepth = SceneLayersLevelRayOpticalDepth(p, cameraZ);
    const float levelRayThinness = std::clamp(1.0f - levelRayOpticalDepth / kFarOpticalDepth, 0.0f, 1.0f);
    return std::max(coverageEdge, levelRayThinness);
}

const AuthoredLayer* MostForwardScatteringLayer(const AuthoredFog& fog) {
    const AuthoredLayer* halo = nullptr;
    for (int i = 0; i < fog.layerCount; ++i) {
        if (!halo || fog.layers[i].g > halo->g) {
            halo = &fog.layers[i];
        }
    }
    return halo;
}

bool HaloHue(const AuthoredFog& fog, float* displayReferredRgb) {
    const AuthoredLayer* halo = MostForwardScatteringLayer(fog);
    if (!halo) {
        return false;
    }
    const float* c = halo->diffuse;
    float peak = std::max(c[0], std::max(c[1], c[2]));
    if (peak < 1e-4f) {
        return false;
    }
    Scale(c, 1.0f / peak, displayReferredRgb);
    return true;
}

// fog_model.cpp:295-312. in.interiorBlend is already 0 unless the camera is in an interior WMO group (coa's
// cameraInterior gate, VolumetricFog.cpp CameraInInteriorGroup).
float InteriorWeight(const FrameInputs& in, const Config& cfg) {
    return cfg.interiorAware && std::isfinite(in.interiorBlend) ? std::clamp(in.interiorBlend, 0.0f, 1.0f) : 0.0f;
}

void ApplyInteriorFog(float weight, const Config& cfg, FogParams& fog) {
    const float retainedDensity =
        std::isfinite(cfg.interiorDensity) ? std::clamp(cfg.interiorDensity, 0.0f, 1.0f) : 0.15f;
    const float densityScale = 1.0f + (retainedDensity - 1.0f) * weight;
    for (int i = 0; i < kSceneLayers; ++i) {
        fog.layers[i].density *= densityScale;
    }
    // coa PR #9 InteriorFarFog: the distance fog keeps this fraction indoors (1 = as before).
    const float retainedFarFog = std::isfinite(cfg.interiorFarFog) ? std::clamp(cfg.interiorFarFog, 0.0f, 1.0f) : 1.0f;
    fog.layers[kDistanceFogLayer].density *= 1.0f + (retainedFarFog - 1.0f) * weight;
    for (FogLayer& layer : fog.layers) {
        Scale(layer.diffuse, 1.0f - weight, layer.diffuse);
    }
    fog.lightVisibility *= 1.0f - weight;
}

float ClientToClassicDirectLight(const AuthoredFog& fog, const float* clientDirectLight, bool linear) {
    if (!fog.hasClassicDirectLight) {
        return 1.0f;
    }
    float classic[3] = { fog.classicDirectLight[0], fog.classicDirectLight[1], fog.classicDirectLight[2] };
    Encode(classic, linear);
    const float classicLuminance = Luminance(classic);
    if (classicLuminance < kMinClassicDirectLuminance) {
        return 1.0f;
    }
    return std::clamp(Luminance(clientDirectLight) / classicLuminance, 0.0f, 1.0f);
}

// ---- fog_model.cpp:450-490 AuthoredNoiseScroll (one instance: the composite's) ----
struct AuthoredNoiseScroll {
    double phaseInTiles[kSceneLayers][kAuthoredNoiseOctaves][3] = {};
    double inverseTileYards[kSceneLayers][kAuthoredNoiseOctaves] = {};
};

AuthoredNoiseScroll s_noiseScroll;

}  // namespace

void UnpackColor(uint32_t argb, float* rgb) {
    rgb[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    rgb[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    rgb[2] = static_cast<float>(argb & 0xFF) / 255.0f;
}

// fog_model.cpp:334-378 (+ wxl-vfog StartOffset, fog_model.cpp:377-380)
FogParams BuildFogParamsFrom(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored) {
    FogParams p = {};
    p.linear = ScattersInLinearLight(cfg);
    p.authored = authored != nullptr;
    const float interiorWeight = InteriorWeight(in, cfg);
    float fogColor[3];
    UnpackColor(in.fogColor, fogColor);
    UnpackColor(in.directColor, p.lightColor);
    UnpackColor(in.lightIsMoon ? in.directColor : in.sunColor, p.rayColor);
    if (in.foreverFog) NoonKeyRow(in.look, p.lightColor);
    Encode(fogColor, p.linear);
    Encode(p.lightColor, p.linear);

    float elevation = SmoothStep(-0.03f, 0.10f, in.toLight[2]);
    p.lightVisibility = elevation * (in.lightIsMoon ? kMoonLightScale : 1.0f);
    p.lightAboveHorizon = SmoothStep(-kLightSettingHalfWidth, kLightSettingHalfWidth, in.toLight[2]);
    p.farClip = in.farClip;
    p.maxDistance = std::max(cfg.maxDistance, in.farClip);
    // FARVIEW: with far land drawing (fogCap past farclip), the horizon band keeps its real distance out to 85 % of
    // the land's reach, as wxl-vfog's far slice did, instead of being fogged as sky from 0.85 x farclip; and the
    // distance layer ends at the stretched stock end (DayNight::UpdateFog), not at farclip.
    const float fogCap = std::max(in.fogCap, in.farClip);
    p.horizonEnd = fogCap > in.farClip && in.horizonBand[3] > in.farClip ? in.horizonBand[3] : in.farClip;
    p.horizonStart = p.horizonEnd * 0.85f;
    p.referenceZ = ReferenceZ(in);
    p.farLimit = std::clamp(in.fogEnd, 100.0f, fogCap);

    float fogDistance = in.zoneFogDistance > 50.0f && in.zoneFogDistance < 20000.0f ? in.zoneFogDistance : in.fogEnd;
    fogDistance = std::clamp(fogDistance, 50.0f, 5000.0f);
    float elevationFadedScatter = p.lightVisibility * cfg.sunScatter;

    FogLayer& distanceFog = p.layers[kDistanceFogLayer];
    p.directLightMatch = 1.0f;
    if (p.authored) {
        p.directLightMatch = ClientToClassicDirectLight(*authored, p.lightColor, p.linear);
        // vfogaudit: elevation- and moon-faded like the derived and distance layers (coa passed the raw scatter, so
        // authored zones had ~3x the moon glow at night and a jump at the sun/moon handover).
        AuthoredLayers(*authored, cfg, p, elevationFadedScatter * p.directLightMatch, p.layers, p.noise);
        HaloHue(*authored, p.rayColor);
        DistanceLayer(in, cfg, p, elevationFadedScatter, fogColor, distanceFog);
        const float thinness = ClassicFogThinness(p, *authored, in.camPos[2]);
        distanceFog.density *= thinness + (1.0f - thinness) * interiorWeight;
    } else {
        DerivedLayers(cfg, p, elevationFadedScatter, fogDistance, fogColor, p.layers);
        DistanceLayer(in, cfg, p, elevationFadedScatter, fogColor, distanceFog);
        if (in.foreverFog) {
            // foreverFog: the haze and ground layers are the same medium as the Forever distance layer (its colour
            // solve and phase), only their density profile differs (the untinted DBC fog colour read grey).
            for (int i = 0; i < 2; ++i) {
                FogLayer& l = p.layers[i];
                std::memcpy(l.emissive, distanceFog.emissive, sizeof(l.emissive));
                std::memcpy(l.diffuse, distanceFog.diffuse, sizeof(l.diffuse));
                l.g = distanceFog.g;
                l.isotropic = distanceFog.isotropic;
                Unshadowed(l);
            }
        }
    }
    ApplyInteriorFog(interiorWeight, cfg, p);
    // wxl-vfog: every layer starts StartOffset yards further out.
    if (std::isfinite(cfg.startOffset) && cfg.startOffset > 0.0f) {
        for (FogLayer& layer : p.layers) {
            layer.start = std::min(layer.start + cfg.startOffset, std::max(p.maxDistance - kFogRangeMargin, 0.0f));
        }
    }
    return p;
}

LightParamsSelection SelectionFor(const FrameInputs& in) {
    LightParamsSelection selection;
    selection.stormBlend = std::isfinite(in.stormBlend) ? std::clamp(in.stormBlend, 0.0f, 1.0f) : 0.0f;
    if (in.screenEffectSlot >= 0 && in.screenEffectSlot < kLightParamsSlots) {
        selection.screenEffectSlot = in.screenEffectSlot;
    }
    return selection;
}

// coa renderer.cpp:1286-1289: Classic data where it covers the camera, derived layers elsewhere.
FogParams BuildFogParams(const FrameInputs& in, const Config& cfg) {
    AuthoredFog authored;
    // foreverFog: BENILLA's one medium. coa's fogdata.bin layers would also zero the Forever distance layer through
    // ClassicFogThinness (grey Barrens noon: the authored layers' colour, no zone haze), so none are resolved.
    const bool hasAuthored =
        cfg.dataMode == 1 && !in.foreverFog && ResolveFogData(in.mapId, in.camPos, in.dayFraction, SelectionFor(in), authored);
    return BuildFogParamsFrom(in, cfg, hasAuthored ? &authored : nullptr);
}

float EnergyNormalisedPhaseScale(float g) {
    const float capped = std::min(g, kMaxEnergyNormalisedPhaseG);
    return (1.0f + capped) / (kFourPi * (1.0f - capped) * (1.0f - capped));
}

bool AnyLayerNoise(const FogParams& fog) {
    for (const LayerNoise& noise : fog.noise) {
        if (noise.alpha > 0.0f) {
            return true;
        }
    }
    return false;
}

float NoiseDensityCurve(float x) {
    const auto belowPivot = [](float v) { return v * v * (kNoiseCurveShape + 1.0f) / (v + kNoiseCurveShape / 2.0f); };
    const float clamped = std::clamp(x, 0.0f, 1.0f);
    return clamped < kNoiseCurvePivot ? belowPivot(clamped) : 1.0f - belowPivot(1.0f - clamped);
}

float MeanNoiseDensity(const LayerNoise& noise) {
    return 1.0f - noise.alpha * (1.0f - kNoiseCurveMean);
}

void MeanNoiseEmissive(const LayerNoise& noise, const float* emissive, float* out) {
    const float thinnedDensity = 1.0f - noise.alpha;
    const float thinnedWeight = (1.0f - kNoiseCurveMean) * thinnedDensity;
    const float totalWeight = kNoiseCurveMean + thinnedWeight;
    for (int c = 0; c < 3; ++c) {
        const float thinnedEmissive = emissive[c] + noise.alpha * (noise.fade[c] - emissive[c]);
        out[c] = totalWeight > 0.0f ? (kNoiseCurveMean * emissive[c] + thinnedWeight * thinnedEmissive) / totalWeight
                                    : emissive[c];
    }
}

FogLayer MeanNoiseLayer(const FogLayer& layer, const LayerNoise& noise, float lightAboveHorizon) {
    FogLayer mean = layer;
    if (noise.alpha <= 0.0f) {
        return mean;
    }
    const float shadow = HorizonShadow(layer, lightAboveHorizon);
    float shadowMixed[3];
    for (int c = 0; c < 3; ++c) {
        shadowMixed[c] = layer.emissive[c] + (layer.shadowEmissive[c] - layer.emissive[c]) * shadow;
    }
    MeanNoiseEmissive(noise, shadowMixed, mean.emissive);
    std::memcpy(mean.shadowEmissive, mean.emissive, sizeof(mean.emissive));
    mean.density *= MeanNoiseDensity(noise);
    return mean;
}

FogParams WithMeanNoise(const FogParams& fog) {
    FogParams mean = fog;
    for (int i = 0; i < kSceneLayers; ++i) {
        mean.layers[i] = MeanNoiseLayer(fog.layers[i], fog.noise[i], fog.lightAboveHorizon);
        mean.noise[i] = LayerNoise{};
    }
    return mean;
}

void AdvanceNoise(const FogParams& fog, const float camPos[3], double seconds) {
    AuthoredNoiseScroll& s = s_noiseScroll;
    for (int layer = 0; layer < kSceneLayers; ++layer) {
        for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
            const double inverseTile = fog.noise[layer].inverseTileYards[octave];
            double& previousInverseTile = s.inverseTileYards[layer][octave];
            for (int axis = 0; axis < 3; ++axis) {
                const double scaledAboutCamera = camPos[axis] * (inverseTile - previousInverseTile);
                const double drift = fog.noise[layer].velocity[octave][axis] * inverseTile * seconds;
                double& phase = s.phaseInTiles[layer][octave][axis];
                phase += scaledAboutCamera + drift;
                phase -= std::floor(phase);
            }
            previousInverseTile = inverseTile;
        }
    }
}

void NoiseRegisters(const FogParams& fog, LayerNoiseRegisters out[kSceneLayers]) {
    const AuthoredNoiseScroll& s = s_noiseScroll;
    for (int layer = 0; layer < kSceneLayers; ++layer) {
        const LayerNoise& noise = fog.noise[layer];
        LayerNoiseRegisters& registers = out[layer];
        registers = LayerNoiseRegisters{};
        for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
            float* octaveRegister = registers.octaveOffsetAndInverseTile[octave];
            octaveRegister[3] = noise.inverseTileYards[octave];
            if (noise.inverseTileYards[octave] <= 0.0f) {
                continue;
            }
            const double tile = 1.0 / noise.inverseTileYards[octave];
            for (int axis = 0; axis < 3; ++axis) {
                octaveRegister[axis] = static_cast<float>(s.phaseInTiles[layer][octave][axis] * tile);
            }
            registers.octaveWeights[octave] = noise.octaveWeight[octave];
        }
        std::memcpy(registers.fadeAndAlpha, noise.fade, sizeof(noise.fade));
        registers.fadeAndAlpha[3] = noise.alpha;
    }
}

// ---- fog_model.cpp:492-551 ----
void Mul4x4(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

bool Invert4x4(const float* m, float* out) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<float>(inv[i] / det);
    }
    return true;
}

void TransformDirection(const float* v, const float* m, float* out) {
    float r[3];
    for (int j = 0; j < 3; ++j) {
        r[j] = v[0] * m[0 * 4 + j] + v[1] * m[1 * 4 + j] + v[2] * m[2 * 4 + j];
    }
    std::memcpy(out, r, sizeof(r));
}

// ===== CPU ray integration: coa-vfog src/transparent_fog.cpp:113-374 =====
namespace {

constexpr float kMaxFitRise = 0.26f;
constexpr float kTaylorOpticalDepth = 1.0e-3f;
constexpr float kMinPhaseDenominator = 1.0e-6f;
constexpr float kDistanceCurveBias = 1.0e-6f;
constexpr int kQuadratureSamples = 2;
constexpr float kFirstQuadratureFraction = 0.2113248654f;
constexpr float kQuadratureFractionSpacing = 0.5773502692f;
constexpr float kQuadratureWeight = 0.5f;
constexpr float kMinLightDistanceSquared = 1.0e-6f;
constexpr float kMinLightFalloffDenominator = 1.0f;
constexpr float kLightBoundaryFadeRate = 4.0f;
constexpr float kMinLightRadius = 1.0e-3f;
constexpr int kMaxPieceCuts = 2 + 2 * kFogLayers + 2 * kMaxLocalLights;

float PhaseHG(float g, float cosAngle) {
    const float r = (1.0f - g) / std::sqrt(std::max(1.0f + g * g - 2.0f * g * cosAngle, kMinPhaseDenominator));
    return r * r * r;
}

float LayerPhase(const FogLayer& layer, float cosAngle) {
    const float hg = PhaseHG(layer.g, cosAngle);
    return hg + (1.0f - hg) * std::fabs(layer.isotropic);   // < 0 = foreverFog's no-floor flag
}

float DistanceCurve(const FogLayer& layer, float distance, float range) {
    const float t = std::clamp(std::max(distance - layer.start, 0.0f) / range, 0.0f, 1.0f);
    return 1.0f + layer.strength * std::pow(t + kDistanceCurveBias, layer.exponent);
}

struct RayLight {
    float cosToLight;
    float lightAboveHorizon;
    float cameraHeight;
    float rise;
    float range;
};

struct LightChord {
    uint32_t light;
    float start;
    float end;
};

struct RayPointLights {
    const FogRayLights* points;
    const float* direction;
    LightChord chords[kMaxLocalLights];
    int chordCount;
};

struct LayerPortion {
    float start;
    float end;
    float density;
};

struct StepLayers {
    LayerPortion portions[kFogLayers];
    int count = 0;
};

void AccumulateLayer(const FogLayer& layer, const RayLight& ray, float stepStart, float stepEnd, float* radiance,
                     float& opticalDepth, StepLayers& stepLayers) {
    const float layerStart = std::max(stepStart, layer.start);
    const float layerLength = std::max(std::min(stepEnd, layer.endDistance) - layerStart, 0.0f);
    if (layerLength <= 0.0f || layer.density <= 0.0f) {
        return;
    }
    const float shadow = layer.shadowed * (1.0f - ray.lightAboveHorizon);
    const float shadowDensity = 1.0f + (layer.shadowDensity - 1.0f) * shadow;
    const float sampleDistance = layerStart + layerLength * 0.5f;
    const float layerOpticalDepth = layer.density * layerLength * DistanceCurve(layer, sampleDistance, ray.range) *
                                    HeightDensityFactor(layer, ray.cameraHeight + ray.rise * sampleDistance) *
                                    shadowDensity;
    stepLayers.portions[stepLayers.count++] = { layerStart, layerStart + layerLength, layerOpticalDepth / layerLength };
    const float directPhase = (1.0f - shadow) * LayerPhase(layer, ray.cosToLight);
    for (int c = 0; c < 3; ++c) {
        const float emissive = layer.emissive[c] + (layer.shadowEmissive[c] - layer.emissive[c]) * shadow;
        radiance[c] += (layer.diffuse[c] * directPhase + emissive) * layerOpticalDepth;
    }
    opticalDepth += layerOpticalDepth;
}

float StepOpacity(float opticalDepth) {
    return opticalDepth < kTaylorOpticalDepth
               ? opticalDepth * (1.0f - 0.5f * opticalDepth + opticalDepth * opticalDepth / 6.0f)
               : 1.0f - std::exp(-opticalDepth);
}

RayPointLights PointLightsAlong(const FogRayLights& points, const float* direction) {
    RayPointLights lit = { &points, direction, {}, 0 };
    for (uint32_t i = 0; i < points.count; ++i) {
        const float* relative = points.relative[i];
        const float radius = points.lights[i].cutoff;
        const float projected = Dot3(relative, direction);
        const float perpendicularSquared = std::max(Dot3(relative, relative) - projected * projected, 0.0f);
        const float discriminant = radius * radius - perpendicularSquared;
        if (discriminant <= 0.0f) {
            continue;
        }
        const float halfChord = std::sqrt(discriminant);
        lit.chords[lit.chordCount++] = { i, projected - halfChord, projected + halfChord };
    }
    return lit;
}

float PointLightScattering(const RayPointLights& lit, uint32_t index, float distance) {
    const LocalLight& light = lit.points->lights[index];
    float toLight[3];
    for (int axis = 0; axis < 3; ++axis) {
        toLight[axis] = lit.points->relative[index][axis] - lit.direction[axis] * distance;
    }
    const float distanceSquared = std::max(Dot3(toLight, toLight), kMinLightDistanceSquared);
    const float lightDistance = std::sqrt(distanceSquared);
    const float* attenuation = light.attenuation;
    const float falloff = 1.0f / std::max(attenuation[0] + attenuation[1] * lightDistance + attenuation[2] * distanceSquared,
                                          kMinLightFalloffDenominator);
    const float boundaryFade = std::clamp(
        (light.cutoff - lightDistance) * kLightBoundaryFadeRate / std::max(light.cutoff, kMinLightRadius), 0.0f, 1.0f);
    return falloff * boundaryFade * PhaseHG(lit.points->phase, Dot3(toLight, lit.direction) / lightDistance);
}

float PortionDensity(const StepLayers& stepLayers, float distance) {
    float density = 0.0f;
    for (int i = 0; i < stepLayers.count; ++i) {
        if (distance > stepLayers.portions[i].start && distance < stepLayers.portions[i].end) {
            density += stepLayers.portions[i].density;
        }
    }
    return density;
}

bool ChordCovers(const LightChord& chord, float distance) {
    return distance > chord.start && distance < chord.end;
}

bool ChordsMeetStep(const RayPointLights& lit, float stepStart, float stepEnd) {
    for (int i = 0; i < lit.chordCount; ++i) {
        if (lit.chords[i].start < stepEnd && lit.chords[i].end > stepStart) {
            return true;
        }
    }
    return false;
}

int PieceCuts(const StepLayers& stepLayers, const RayPointLights& lit, float stepStart, float stepEnd, float* cuts) {
    int count = 0;
    cuts[count++] = stepStart;
    cuts[count++] = stepEnd;
    const auto cutInside = [&](float cut) {
        if (cut > stepStart && cut < stepEnd) {
            cuts[count++] = cut;
        }
    };
    for (int i = 0; i < stepLayers.count; ++i) {
        cutInside(stepLayers.portions[i].start);
        cutInside(stepLayers.portions[i].end);
    }
    for (int i = 0; i < lit.chordCount; ++i) {
        cutInside(lit.chords[i].start);
        cutInside(lit.chords[i].end);
    }
    std::sort(cuts, cuts + count);
    return count;
}

void AccumulatePointLightPiece(const StepLayers& stepLayers, const RayPointLights& lit, float from, float to,
                               float* radiance) {
    const float length = to - from;
    const float middle = from + 0.5f * length;
    const float weightedDensity = kQuadratureWeight * length * PortionDensity(stepLayers, middle);
    if (weightedDensity <= 0.0f) {
        return;
    }
    for (int i = 0; i < lit.chordCount; ++i) {
        if (!ChordCovers(lit.chords[i], middle)) {
            continue;
        }
        const uint32_t light = lit.chords[i].light;
        float scattering = 0.0f;
        for (int sample = 0; sample < kQuadratureSamples; ++sample) {
            const float fraction = kFirstQuadratureFraction + static_cast<float>(sample) * kQuadratureFractionSpacing;
            scattering += PointLightScattering(lit, light, from + length * fraction);
        }
        const float* colour = lit.points->lights[light].color;
        for (int c = 0; c < 3; ++c) {
            radiance[c] += colour[c] * weightedDensity * scattering;
        }
    }
}

void AccumulatePointLights(const StepLayers& stepLayers, const RayPointLights& lit, float stepStart, float stepEnd,
                           float* radiance) {
    if (stepLayers.count == 0 || !ChordsMeetStep(lit, stepStart, stepEnd)) {
        return;
    }
    float cuts[kMaxPieceCuts];
    const int cutCount = PieceCuts(stepLayers, lit, stepStart, stepEnd, cuts);
    for (int piece = 0; piece + 1 < cutCount; ++piece) {
        if (cuts[piece + 1] > cuts[piece]) {
            AccumulatePointLightPiece(stepLayers, lit, cuts[piece], cuts[piece + 1], radiance);
        }
    }
}

}  // namespace

FogRayLights RayLightsFor(const FrameInputs& in, uint32_t count, float phase) {
    FogRayLights points;
    points.lights = in.localLights;
    const uint32_t available = in.localLightCount > 0 ? static_cast<uint32_t>(in.localLightCount) : 0u;
    points.count = std::min({ count, available, static_cast<uint32_t>(kMaxLocalLights) });
    points.phase = phase;
    for (uint32_t i = 0; i < points.count; ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            points.relative[i][axis] = points.lights[i].position[axis] - in.camPos[axis];
        }
    }
    return points;
}

void IntegrateFogRay(const FogParams& fog, const FogRayLights& lights, const FogRay& fogRay, const FrameInputs& in,
                     float stepDepth, int steps, FogSample* samples) {
    const RayLight ray = { Dot3(in.toLight, fogRay.world), fog.lightAboveHorizon, in.camPos[2],
                           std::clamp(fogRay.world[2], -kMaxFitRise, kMaxFitRise), fog.maxDistance };
    const RayPointLights lit = PointLightsAlong(lights, fogRay.world);
    float transmittance = 1.0f;
    float inScatter[3] = {};
    samples[0] = { 1.0f, { 0.0f, 0.0f, 0.0f } };
    for (int step = 0; step < steps; ++step) {
        const float stepStart = static_cast<float>(step) * stepDepth * fogRay.distancePerViewDepth;
        const float stepEnd = static_cast<float>(step + 1) * stepDepth * fogRay.distancePerViewDepth;
        float radiance[3] = {};
        float opticalDepth = 0.0f;
        StepLayers stepLayers;
        for (const FogLayer& layer : fog.layers) {
            AccumulateLayer(layer, ray, stepStart, stepEnd, radiance, opticalDepth, stepLayers);
        }
        AccumulatePointLights(stepLayers, lit, stepStart, stepEnd, radiance);
        if (opticalDepth > 0.0f) {
            const float opacity = StepOpacity(opticalDepth);
            for (int c = 0; c < 3; ++c) {
                inScatter[c] += transmittance * radiance[c] * (opacity / opticalDepth);
            }
            transmittance *= 1.0f - opacity;
        }
        samples[step + 1] = { transmittance, { inScatter[0], inScatter[1], inScatter[2] } };
    }
}

// ---- foreverFog zone looks: BENILLA modern/look.rs rows 1:1 (fog fields only; the rest of each row is grading,
// exposure and lamp/bank data we do not port). AreaTable ids checked against our 3.3.5 AreaTable.dbc (same ids;
// Outland and Northrend have no row and take the default look).
namespace {
//                               forward keyScat glow                 hazeTint               reach  fogTint               fall   clear  moonGlow moonHue skyTint  silh noonMix
constexpr ForeverLook kNeutral = { 0.72f, 0.7f, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 0.5f, { 1.0f, 1.0f, 1.0f }, 70.0f, 450.0f, 1.0f, 1.0f, { 1.0f, 1.0f, 1.0f }, 0.55f, 0.0f };
ForeverLook Sky(ForeverLook l, float r, float g, float b) {   // look.rs sky_tint
    l.skyTint[0] = r; l.skyTint[1] = g; l.skyTint[2] = b;
    return l;
}
ForeverLook With(ForeverLook l, const float* glow, const float* haze, const float* tint) {
    for (int c = 0; c < 3; ++c) {
        if (glow) l.glow[c] = glow[c];
        if (haze) l.hazeTint[c] = haze[c];
        if (tint) l.fogTint[c] = tint[c];
    }
    return l;
}
ForeverLook Noon() {                    // ZoneLook::NOON
    const float haze[3] = { 0.8f, 1.0f, 1.05f };
    ForeverLook l = With(kNeutral, nullptr, haze, nullptr);
    l.noonMix = 1.0f;
    return l;
}
ForeverLook Eerie() {                   // ZoneLook::EERIE
    const float glow[3] = { 0.65f, 1.0f, 0.7f }, haze[3] = { 0.22f, 0.34f, 0.26f }, tint[3] = { 0.8f, 1.0f, 0.8f };
    return Sky(With(kNeutral, glow, haze, tint), 0.6f, 0.7f, 0.6f);
}
ForeverLook Tirisfal() {                // ZoneLook::TIRISFAL
    const float glow[3] = { 0.58f, 1.0f, 0.62f }, haze[3] = { 0.19f, 0.36f, 0.22f }, tint[3] = { 0.7f, 1.05f, 0.7f };
    return Sky(With(kNeutral, glow, haze, tint), 0.52f, 0.72f, 0.52f);
}
ForeverLook Cold() {                    // ZoneLook::COLD
    const float glow[3] = { 0.72f, 0.88f, 1.18f }, haze[3] = { 0.42f, 0.68f, 1.28f }, tint[3] = { 0.58f, 0.82f, 1.32f };
    ForeverLook l = With(kNeutral, glow, haze, tint);
    l.forward = 0.6f; l.keyScatter = 0.6f; l.fogReach = 0.38f; l.fogClear = 250.0f;
    return Sky(l, 0.6f, 0.84f, 1.32f);
}
ForeverLook ColdNight() {               // ZoneLook::COLD_NIGHT
    ForeverLook l = Cold();
    l.fogReach = 1.2f;
    return l;
}
ForeverLook Ironforge() {               // ZoneLook::IRONFORGE
    const float haze[3] = { 1.0f, 0.92f, 0.82f };
    ForeverLook l = With(kNeutral, nullptr, haze, nullptr);
    l.keyScatter = 0.4f; l.fogReach = 1.5f; l.fogClear = 900.0f;
    return l;
}
ForeverLook Darkshore() {               // ZoneLook::DARKSHORE
    const float glow[3] = { 1.15f, 0.7f, 0.6f }, haze[3] = { 0.85f, 0.76f, 0.78f }, tint[3] = { 2.1f, 1.85f, 1.85f };
    ForeverLook l = With(kNeutral, glow, haze, tint);
    l.forward = 0.82f; l.keyScatter = 0.9f; l.fogReach = 0.7f; l.fogFalloff = 100.0f; l.fogClear = 80.0f;
    l.moonGlow = 0.1f; l.moonHue = 0.0f; l.silhouette = 0.75f;
    return Sky(l, 1.15f, 1.0f, 1.0f);
}
ForeverLook DarkshoreNoon() {           // ZoneLook::DARKSHORE_NOON
    const float glow[3] = { 1.0f, 0.96f, 0.95f }, haze[3] = { 0.8f, 0.8f, 0.82f }, tint[3] = { 1.9f, 1.9f, 1.93f };
    return Sky(With(Darkshore(), glow, haze, tint), 1.0f, 1.0f, 1.04f);
}

bool In(uint32_t zone, std::initializer_list<uint32_t> ids) {
    return std::find(ids.begin(), ids.end(), zone) != ids.end();
}

// sun_arc.rs midday_at (MIDDAY_IN 540..660, MIDDAY_OUT 870..990) and night_at (moonrise 1236.5, moonset 366.5,
// STARS_FADE 60).
float MiddayAt(float minute) {
    auto ramp = [minute](float a, float b) {
        const float t = std::clamp((minute - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    return ramp(540.0f, 660.0f) * (1.0f - ramp(870.0f, 990.0f));
}
float NightAt(float minute) {
    auto wrap = [](float x) { return std::fmod(std::fmod(x, 1440.0f) + 1440.0f, 1440.0f); };
    const float afterRise = wrap(minute - 1236.5f), beforeSet = wrap(366.5f - minute);
    const float night = 1440.0f - (1236.5f - 366.5f);
    return afterRise <= night ? std::clamp(std::min(afterRise, beforeSet) / 60.0f, 0.0f, 1.0f) : 0.0f;
}
}  // namespace

void ForeverSilhouetteColor(const FrameInputs& in, float rgb[3]) {
    UnpackColor(in.fogColor, rgb);
    const float interior = std::clamp(in.interiorBlend, 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) rgb[c] *= 1.0f + (in.look.fogTint[c] - 1.0f) * (1.0f - interior);
    Subtle(rgb);
    for (int c = 0; c < 3; ++c) rgb[c] = std::min(rgb[c] * std::max(in.look.silhouette, 0.0f), 1.0f);
}

ForeverLook ForeverNeutralLook() {
    return kNeutral;
}

ForeverLook LerpLook(const ForeverLook& a, const ForeverLook& b, float t) {
    auto m = [t](float x, float y) { return x + (y - x) * t; };
    ForeverLook o;
    o.forward = m(a.forward, b.forward);
    o.keyScatter = m(a.keyScatter, b.keyScatter);
    for (int c = 0; c < 3; ++c) {
        o.glow[c] = m(a.glow[c], b.glow[c]);
        o.hazeTint[c] = m(a.hazeTint[c], b.hazeTint[c]);
        o.fogTint[c] = m(a.fogTint[c], b.fogTint[c]);
        o.skyTint[c] = m(a.skyTint[c], b.skyTint[c]);
    }
    o.fogReach = m(a.fogReach, b.fogReach);
    o.fogFalloff = m(a.fogFalloff, b.fogFalloff);
    o.fogClear = m(a.fogClear, b.fogClear);
    o.moonGlow = m(a.moonGlow, b.moonGlow);
    o.moonHue = m(a.moonHue, b.moonHue);
    o.silhouette = m(a.silhouette, b.silhouette);
    o.noonMix = m(a.noonMix, b.noonMix);
    return o;
}

// look.rs look_for_zone_at: the default look ramps into NOON across midday, Darkshore into DARKSHORE_NOON, the snow
// zones into COLD_NIGHT across the night; the others keep their row at every hour.
ForeverLook ForeverLookForZone(uint32_t zone, float minute) {
    if (zone == 85) return Tirisfal();
    if (In(zone, { 10, 130, 41, 28, 139, 8, 361, 5179, 5208 })) return Eerie();
    if (zone == 1537) return Ironforge();
    if (zone == 148) return LerpLook(Darkshore(), DarkshoreNoon(), MiddayAt(minute));
    if (In(zone, { 1, 618, 2597, 36 })) return LerpLook(Cold(), ColdNight(), NightAt(minute));
    return LerpLook(kNeutral, Noon(), MiddayAt(minute));
}

}  // namespace VolumetricFog
