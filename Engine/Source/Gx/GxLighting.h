#ifndef ENGINE_SOURCE_GX_GXLIGHTING_H
#define ENGINE_SOURCE_GX_GXLIGHTING_H

// CUSTOM (Badgermilk0 "modern lighting" port, post passes only): not in the 3.3.5a binary. The game
// (WoW/Source/Ui/WorldLighting.cpp) describes each world frame; the D3D11 and Vulkan devices draw it and set
// g_gxLighting while they are up. D3D9 leaves it null and nothing changes.
//
// The calls bracket the world pass in CGWorldFrame::OnWorldRender:
//   BeginWorld   before CWorld::Render: the world draws into the HDR scene target (hdr on, D3D11)
//   SetupView    in CWorldScene::Render once the camera is set: the view and projection the passes use
//   OpaqueDone   after the opaque models and the grass, before liquids and everything transparent: SSAO and fog
//   EndWorld     before FFX::Finish: bloom and the tonemap into the frame

#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"

enum EGxLightingFeature : uint32_t {
    GxLighting_PerPixel = 0x1,        // M2, WMO and terrain lit per pixel with their stock formulas (lit shader pack)
    GxLighting_Dynamic = 0x2,         // the point lights in GxLightingGpu
    GxLighting_Specular = 0x4,        // WMO specular shaders per pixel, a highlight on sphere-mapped M2s
    GxLighting_Hemisphere = 0x8,      // the ambient tinted by the sky above and the ground below
    GxLighting_Ssao = 0x10,
    GxLighting_HeightFog = 0x20,
    GxLighting_Scattering = 0x40,     // the fog tinted toward the sun's colour around the sun
    GxLighting_Hdr = 0x80,            // the world into a 16-bit float target, tonemapped
    GxLighting_Bloom = 0x100,         // needs Hdr
    GxLighting_Pbr = 0x200,           // PBR-lite: GGX specular, sky reflection, energy conservation (lightPbr)
    GxLighting_SkyOcclusion = 0x400,  // BENILLA sky occlusion (D3D11): the frame dimmed under the world's tops (skyOcclusion)
    GxLighting_Wet = 0x800,           // BENILLA rain on surfaces (D3D11): wet darkening, puddles, reflections (rainWetness)
    GxLighting_ProbeGi = 0x2000,      // BENILLA probe GI (D3D11): the stock ambient swapped for a probe field's irradiance (probeGi)
    GxLighting_FarSeam = 0x1000,      // BENILLA far view seam (D3D11): the detailed world dithers into the far land (farLandSeam)
};

// CUSTOM (PBR-lite): the material category of the next draw, guessed by the engine (assets carry no PBR maps). The
// devices hand it to the lit pixel shader with the lit flag: D3D11 dev_alphaRef.y, Vulkan regs.misc.y = 1 + category
// (0 = the bound VS does not write the surface). Only M2 draws set it; the shaders read it for M2 surfaces only.
enum EGxLightingMaterial : uint32_t {
    GxLightingMaterial_Generic = 0,   // a doodad: wood, stone, plants (rough)
    GxLightingMaterial_Skin = 1,      // character and creature bodies, hair (medium)
    GxLightingMaterial_Item = 2,      // item models and textures: weapons, unknown items (metal on grey, bright texels)
    // ARMORMETAL: equipped armour by its Item.dbc subclass (CCharacterComponent: attachments by their item, the
    // composite body skin by the item that painted each section)
    GxLightingMaterial_Cloth = 3,     // cloth and leather armour, capes (non-metal)
    GxLightingMaterial_Mail = 4,      // lightMetalMail
    GxLightingMaterial_Plate = 5,     // lightMetalPlate
    GxLightingMaterial_Masked = 6,    // ARMORMASK: a metal mask is bound on kArmorMaskStage (ArmorMask.h: R plate, G mail,
                                      // B roughness; the composite body skin, a helm or shoulders)
};

// ARMORMASK: the texture stage the armour metal mask rides (CM2SceneRender::DrawBatch); the translated lit M2 pixel
// shaders declare sampler s11. Stages 4-7 are the ShadowCache maps', 10 the WMO normal map's (10-13 terrain heights).
constexpr uint32_t kArmorMaskStage = 11;

extern uint32_t g_gxLightingMaterial;

struct GxLightingParams {
    uint32_t m_features = 0;

    // HDR and tonemap
    float m_exposure = 1.0f;
    uint32_t m_tonemap = 1;          // 0 clamp (stock), 1 neutral (stock below a shoulder), 2 ACES, 3 Reinhard, 4 Hable, 5 AgX, 6 AgX punchy, 7 PBR Neutral, 8 Uchimura, 9 Lottes
    float m_bloomIntensity = 0.0f;
    float m_bloomThreshold = 1.0f;   // linear
    // BENILLA auto-exposure (D3D11, hdr): the screen metered, a dark picture lifted toward visible, never darkened
    float m_autoExposureMax = 0.0f;  // the lift's ceiling, 0 = off
    float m_autoExposureTarget = 0.06f;  // the exposed scene key (log-average linear luminance) below which it lifts

    // SSAO
    float m_ssaoIntensity = 0.0f;
    float m_ssaoRadius = 1.5f;       // yards
    float m_ssaoDistance = 200.0f;   // fades out to here
    // BENILLA SSILVB (D3D11): replaces the SSAO pass; AO strength is m_ssaoIntensity
    bool m_ssilvb = false;
    float m_ssgiIntensity = 0.0f;    // the indirect (colour bounce) half, 0 = the AO-only variant
    float m_ssilvbRadius = 4.0f;     // yards
    float m_ssilvbThickness = 0.6f;  // yards, the depth a sample is taken to have
    // BENILLA sky occlusion (D3D11): the top-down map of the world's tops, WoW/Source/WorldClient/SkyOcclusion.h
    const float* m_skyHeights = nullptr;   // m_skyRow.w^2 world z, row-major by world y
    uint32_t m_skySerial = 0;              // moves when m_skyHeights changed
    float m_skyRow[4] = {};                // x0, y0 (render-relative), 1 / texel yd, texels a side
    float m_skyParams[4] = {};             // floor, lift (yd), lift per yd off the texel, render origin z
    // BENILLA probe GI (D3D11): WoW/Source/WorldClient/ProbeGi.h
    const float* m_giProbes = nullptr;     // m_giSize float4 texels
    uint32_t m_giSerial = 0;
    uint32_t m_giSize[2] = {};             // texels wide, high
    float m_giGrid[3][4] = {};             // per cascade: probe 0,0,0 (render-relative), spacing
    float m_giParams[4] = {};              // strength, normal bias (spacings), chebyshev, 0
    float m_giAmbient[4] = {};             // the stock ambient rgb the probes replace
    // BENILLA rain on surfaces (D3D11; weather/wetness.rs): m_skyHeights doubles as the shelter map when set
    float m_wetness = 0.0f;                // 0 dry .. 1 soaked
    bool m_wetReflections = true;          // the screen-space reflection (else the fog colour)
    float m_renderOrigin[3] = {};          // world position of the render origin

    // the lit shaders (GxLightingGpu)
    float m_lightWrap = 0.25f;
    float m_dynamicIntensity = 1.0f;
    float m_specularM2 = 0.3f;
    float m_specularWmo = 1.0f;
    float m_wmoNormal = 0.0f;        // WMO normal map strength, 0 = off (GxLightingGpu m_groundTint[3])
    C3Vector m_skyColor;             // 0 to 1, gamma as the engine stores them
    C3Vector m_groundColor;
    float m_hemisphere = 0.0f;
    float m_roughnessBias = 0.0f;    // PBR-lite: added to every surface's roughness
    float m_metalPlate = 0.0f;       // ARMORMETAL: plate armour's metalness (lightMetalPlate)
    float m_metalMail = 0.0f;        // mail's (lightMetalMail)

    // fog, as the engine has it this frame (DayNight)
    C3Vector m_fogColor;
    float m_fogStart = 0.0f;
    float m_fogEnd = 0.0f;
    float m_fogDensity = 1.0f;
    C3Vector m_sunColor;
    C3Vector m_sunDirection;         // world, the way the light travels
    float m_scattering = 0.0f;
    float m_heightFogDensity = 0.0f; // per yard at the base height
    float m_heightFogFalloff = 0.05f;
    float m_heightFogBase = 0.0f;    // world z (render-relative once SetupView has it)
    float m_heightFogMax = 0.9f;
    C3Vector m_cameraPosition;       // world (render-relative once SetupView has it)
    float m_time = 0.0f;             // seconds
    uint32_t m_debug = 0;            // 1 the SSAO term alone, 2 lit surfaces by kind, 3 dynamic light alone, 4 and 5 the SSAO pass's inputs
};

// CUSTOM (Badgermilk0 port, the shader half): what the lit world shaders read. The translated shader pack
// (shader-xlate sm3hlsl.py for D3D11, Tools/VulkanShaders/bls/emitter.py for Vulkan) lets the M2, WMO and terrain
// vertex shaders hand their view-space surface to the pixel shader, which lights it again per pixel and adds these
// lights. Layout is std140 and HLSL cbuffer alike (every member a 16-byte row):
//   D3D11: cbuffer GxLighting : register(b3) in VS and PS; the vertex constants (VS cb0) also at PS b4
//   Vulkan: uniform block set 1 binding 1 (the pixel module reads the vertex constants from regs.v[])
// The game (WoW/Source/Ui/WorldLighting.cpp) fills g_gxLightingGpu at SetupView and zeroes m_flags[0] at EndWorld,
// bumping g_gxLightingGpuSerial each time; a device uploads it when the serial changes. m_flags[0] == 0 (the default,
// and every frame with the shader CVars off) = every lit shader takes its stock path: the frame is the stock one.
// Per draw, the device tells the pixel shader whether the bound vertex shader writes the surface (CGxShader::
// m_litSurface): D3D11 cb1 dev_alphaRef.y, Vulkan regs.misc.y, 1 or 0. A lit pixel shader relights only when it is 1.
constexpr uint32_t kGxLightingMaxLights = 32;

// WMONORMALS: the texture stage a WMO batch's normal map rides (CMapObj::BindNormalMap); the translated MapObj
// pixel shaders declare sampler s10. Stages 4-7 are the ShadowCache maps'.
constexpr uint32_t kWmoNormalStage = 10;

struct GxLightingLight {
    C3Vector m_position;  // world, render-relative (as the view matrix)
    float m_radius;       // no light from here on
    float m_start;        // full light up to here
    C3Vector m_color;     // linear, intensity included
    float m_flicker;      // 0 to 1
    float m_specular;     // 0 to 1
};

struct GxLightingGpuLight {
    float m_pos[3];       // view space
    float m_radius;
    float m_color[3];     // linear
    float m_start;
    float m_flicker;
    float m_seed;         // 0 to 2 pi, from the world position
    float m_specular;
    float m_pad;
};

struct GxLightingGpu {
    uint32_t m_flags[4];      // x GxLighting_PerPixel..Hemisphere, Pbr (0 = off), y lights, z roughness bias (float bits), w lightingDebug (2, 3)
    float m_view[16];         // row-major (row vector * matrix): render-relative world -> view, for the terrain
    float m_viewInv[16];
    float m_skyTint[4];       // rgb sky colour at luminance 1, a hemisphere strength
    float m_groundTint[4];    // rgb ground colour at luminance 1, a WMO normal map strength (wmoNormalStrength, 0 = off)
    float m_upView[4];        // xyz world up in view space, w time in seconds (flicker)
    float m_lightParams[4];   // x wrap, y dynamic intensity, z M2 specular, w WMO specular
    GxLightingGpuLight m_lights[kGxLightingMaxLights];
    float m_armor[4];         // ARMORMETAL: x plate metalness, y plate roughness, z mail metalness, w mail roughness
};

extern GxLightingGpu g_gxLightingGpu;
extern uint32_t g_gxLightingGpuSerial;

class IGxLighting {
    public:
    virtual void BeginWorld(const GxLightingParams& params) = 0;
    virtual void SetupView(const GxLightingParams& params) = 0;
    virtual void OpaqueDone() = 0;
    virtual void EndWorld() = 0;
    // BENILLA far view seam: after the terrain and WMO groups, before the far trees and the horizon. Pixels whose view
    // depth lies in [from, to] are pushed behind the horizon band by a dithered share smoothstep(from, to, depth), so
    // the far land draws there. Default: nothing (Vulkan).
    virtual void FarSeam(float from, float to) {
        (void)from;
        (void)to;
    }
};

extern IGxLighting* g_gxLighting;

#endif
