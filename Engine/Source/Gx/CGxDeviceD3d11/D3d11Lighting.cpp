#include "Engine/Source/Gx/CGxDeviceD3d11/D3d11Lighting.h"
#include <cmath>
#include <cstring>
#include <utility>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/D3d11Util.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/LightingShaders.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/GxLighting.h"
#include "Engine/Source/Tempest/C44Matrix.h"

// CUSTOM: see D3d11Lighting.h. Ported from Badgermilk0 D3d11Lighting.cpp (feature/modern-lighting), minus the light
// clusters and the lighting constants of the translated shaders.

namespace {

// the passes' cbuffer, b0 (Lighting.hlsl GxPost)
struct PostConstants {
    float m_invProj[16];
    float m_invView[16];
    float m_viewport[4];
    float m_depthRange[4];
    float m_target[4];
    float m_ao[4];
    float m_aoProj[4];
    float m_fogColor[4];
    float m_fogParams[4];
    float m_sunColor[4];
    float m_sunDir[4];
    float m_heightFog[4];
    float m_camera[4];
    float m_bloom[4];
    float m_tone[4];
    float m_source[4];
    float m_projW[4];
    float m_ssilvb[4];
    float m_skyRow[4];
    float m_sky[4];
    float m_wet[4];
    float m_origin[4];
    float m_projFwd[16];
};

// the order of Lighting.py's JOBS
enum EShader {
    Shader_FullscreenVs = 0,
    Shader_AoPs,
    Shader_AoPsMs,
    Shader_BlurPs,
    Shader_AoCompositePs,
    Shader_AoCompositePsMs,
    Shader_FogPs,
    Shader_FogPsMs,
    Shader_BloomPrefilterPs,
    Shader_BloomDownPs,
    Shader_BloomUpPs,
    Shader_TonemapPs,
    Shader_MeterPs,
    Shader_SsilvbPs,
    Shader_SsilvbPsMs,
    Shader_SsilvbAoPs,
    Shader_SsilvbAoPsMs,
    Shader_SsilvbBlurPs,
    Shader_SsilvbBlurPsMs,
    Shader_SsilvbCompositePs,
    Shader_SkyOcclusionPs,
    Shader_SkyOcclusionPsMs,
    Shader_WetDarkenPs,
    Shader_WetDarkenPsMs,
    Shader_WetReflectPs,
    Shader_WetReflectPsMs,
    Shaders_Last,
};

static_assert(sizeof(g_lightingShaders) / sizeof(g_lightingShaders[0]) == Shaders_Last, "LightingShaders.h is stale");

struct Target {
    ID3D11Texture2D* m_texture = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    ID3D11ShaderResourceView* m_srv = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

const uint32_t s_bloomLevels = 6;

// BENILLA auto-exposure (benilla-bourbon modern/meter.rs + modern/look.rs), constants 1:1
const uint32_t s_meterWidth = 64;
const uint32_t s_meterHeight = 36;
const float s_autoExposureStrength = 0.65f;  // about two thirds of the shortfall, in stops
const float s_autoExposureSeconds = 1.5f;    // an eye adapting, not a meter snapping
const float s_meterStaleSeconds = 1.0f;      // no new reading for this long: the lift holds

class D3d11Lighting final : public IGxLighting {
    public:
    explicit D3d11Lighting(CGxDeviceD3d11* device) : m_device(device) {
    }

    void BeginWorld(const GxLightingParams& params) override;
    void SetupView(const GxLightingParams& params) override;
    void OpaqueDone() override;
    void EndWorld() override;
    void ReleaseTargets();
    void ReleaseAll();
    ID3D11DeviceContext* Context() const {
        return this->m_device->m_d3dContext;
    }

    private:
    CGxDeviceD3d11* m_device;
    GxLightingParams m_params;
    bool m_worldActive = false;
    bool m_viewSet = false;
    bool m_swapped = false;     // the scene is in the device's back-buffer members
    bool m_failed = false;

    // the HDR scene, in the device's back-buffer slot layout: texture, view, resolve (MSAA), SRV of the resolved one
    ID3D11Texture2D* m_scene = nullptr;
    ID3D11RenderTargetView* m_sceneRtv = nullptr;
    ID3D11Texture2D* m_sceneResolve = nullptr;
    ID3D11ShaderResourceView* m_sceneSrv = nullptr;
    bool m_sceneResolveValid = false;
    uint32_t m_sceneWidth = 0;
    uint32_t m_sceneHeight = 0;
    uint32_t m_sceneSamples = 0;

    Target m_ao[2];
    Target m_ssilvb[2];
    Target m_bloom[s_bloomLevels];

    // the auto-exposure: the meter grid, its one readback in flight, the last scene key and the adapted lift
    // BENILLA sky occlusion: the top-down heights (R32F), uploaded when their serial moves
    ID3D11Texture2D* m_skyTex = nullptr;
    ID3D11ShaderResourceView* m_skySrv = nullptr;
    uint32_t m_skySize = 0;
    uint32_t m_skySerial = 0;
    bool ISkyMap();
    // BENILLA rain on surfaces: the opaque frame's snapshot (mipped: level 2 is the reflection's blur)
    ID3D11Texture2D* m_wetTex = nullptr;
    ID3D11ShaderResourceView* m_wetSrv = nullptr;
    bool m_wetShelter = false;
    void IWet(ID3D11ShaderResourceView* depth, bool msaa, bool shelter);

    Target m_meter;
    ID3D11Texture2D* m_meterStaging = nullptr;
    bool m_meterPending = false;
    float m_sceneKey = 0.0f;
    float m_sceneKeyAge = 1e9f;
    float m_eye = 1.0f;
    bool m_eyeSet = false;
    float m_lastTime = -1.0f;
    ID3D11DeviceChild* m_shaders[Shaders_Last] = {};
    ID3D11Buffer* m_constants = nullptr;

    D3D11_VIEWPORT m_viewport = {};
    float m_viewInv[16] = {};
    float m_proj[16] = {};
    float m_projInv[16] = {};

    bool ICreate();
    bool IEnsureScene(uint32_t width, uint32_t height, uint32_t samples);
    bool IEnsureTarget(Target& target, uint32_t width, uint32_t height, DXGI_FORMAT format);
    void ISwap();
    ID3D11ShaderResourceView* ISceneSrv();
    ID3D11BlendState* IBlend(D3D11_BLEND src, D3D11_BLEND dst, uint8_t mask);
    void IWriteConstants(uint32_t targetWidth, uint32_t targetHeight, const float* source);
    void IDraw(EShader shader, ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport, ID3D11BlendState* blend, ID3D11ShaderResourceView* const* srvs, uint32_t count);
    void IRestoreDevice();
    void ISsilvb(ID3D11ShaderResourceView* depth, bool msaa);
    void IAutoExposure(ID3D11ShaderResourceView* scene, uint32_t width, uint32_t height);
};

D3d11Lighting* s_lighting = nullptr;
// the lit world shaders' cbuffer GxLighting (b3, GxLighting.h): g_gxLightingGpu, uploaded when its serial moves. Created
// zeroed with the device (everything off = the stock frame), released with it.
ID3D11Buffer* s_gpuConstants = nullptr;
uint32_t s_gpuSerial = 0;

void ReleaseTarget(Target& target) {
    SafeRelease(target.m_srv);
    SafeRelease(target.m_rtv);
    SafeRelease(target.m_texture);
    target.m_width = 0;
    target.m_height = 0;
}

bool D3d11Lighting::ICreate() {
    if (this->m_failed) {
        return false;
    }

    if (this->m_constants) {
        return true;
    }

    ID3D11Device* device = this->m_device->m_d3dDevice;
    bool ok = SUCCEEDED(device->CreateVertexShader(g_lightingShaders[0].data, g_lightingShaders[0].size, nullptr, reinterpret_cast<ID3D11VertexShader**>(&this->m_shaders[0])));
    // the MSAA variants are ps_4_1: below 10_1 they stay null and the passes skip a multisampled frame
    bool ps41 = this->m_device->m_featureLevel >= D3D_FEATURE_LEVEL_10_1;

    for (uint32_t i = 1; ok && i < Shaders_Last; i++) {
        bool msaa = i == Shader_AoPsMs || i == Shader_AoCompositePsMs || i == Shader_FogPsMs || i == Shader_SsilvbPsMs || i == Shader_SsilvbAoPsMs || i == Shader_SsilvbBlurPsMs;

        if (msaa && !ps41) {
            continue;
        }

        ok = SUCCEEDED(device->CreatePixelShader(g_lightingShaders[i].data, g_lightingShaders[i].size, nullptr, reinterpret_cast<ID3D11PixelShader**>(&this->m_shaders[i])));
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(PostConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&desc, nullptr, &this->m_constants));

    if (!ok) {
        D3d11Log("D3d11Lighting: the post shaders failed to create; lighting stays off");
        this->ReleaseAll();
        this->m_failed = true;
    }

    return ok;
}

bool D3d11Lighting::IEnsureTarget(Target& target, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    width = width ? width : 1;
    height = height ? height : 1;

    if (target.m_texture && target.m_width == width && target.m_height == height) {
        return true;
    }

    ReleaseTarget(target);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device* device = this->m_device->m_d3dDevice;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.m_texture)) || FAILED(device->CreateRenderTargetView(target.m_texture, nullptr, &target.m_rtv)) || FAILED(device->CreateShaderResourceView(target.m_texture, nullptr, &target.m_srv))) {
        ReleaseTarget(target);
        return false;
    }

    target.m_width = width;
    target.m_height = height;
    return true;
}

bool D3d11Lighting::IEnsureScene(uint32_t width, uint32_t height, uint32_t samples) {
    if (this->m_scene && this->m_sceneWidth == width && this->m_sceneHeight == height && this->m_sceneSamples == samples) {
        return true;
    }

    SafeRelease(this->m_sceneSrv);
    SafeRelease(this->m_sceneResolve);
    SafeRelease(this->m_sceneRtv);
    SafeRelease(this->m_scene);
    this->m_sceneSamples = 0;

    ID3D11Device* device = this->m_device->m_d3dDevice;
    UINT quality = 0;

    if (samples > 1 && (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, samples, &quality)) || !quality)) {
        D3d11LogOnce("D3d11Lighting: no %u-sample R16G16B16A16_FLOAT target; hdr stays off", samples);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &this->m_scene)) || FAILED(device->CreateRenderTargetView(this->m_scene, nullptr, &this->m_sceneRtv))) {
        D3d11LogOnce("D3d11Lighting: no HDR scene target (%u x %u, %u samples)", width, height, samples);
        SafeRelease(this->m_sceneRtv);
        SafeRelease(this->m_scene);
        return false;
    }

    this->m_sceneWidth = width;
    this->m_sceneHeight = height;
    this->m_sceneSamples = samples;
    return true;
}

// The scene and the back buffer trade places in the device. The resolve texture and SRV are made lazily by
// IResolvedBackBuffer / IBlitBackBuffer on whichever stands there, so they travel with it.
void D3d11Lighting::ISwap() {
    CGxDeviceD3d11* dev = this->m_device;
    std::swap(dev->m_backBuffer, this->m_scene);
    std::swap(dev->m_backBufferRtv, this->m_sceneRtv);
    std::swap(dev->m_resolveTexture, this->m_sceneResolve);
    std::swap(dev->m_backBufferSrv, this->m_sceneSrv);
    std::swap(dev->m_resolveValid, this->m_sceneResolveValid);
    this->m_swapped = !this->m_swapped;
}

// the single-sampled scene (resolved when multisampled), once it is out of the device again
ID3D11ShaderResourceView* D3d11Lighting::ISceneSrv() {
    ID3D11Device* device = this->m_device->m_d3dDevice;
    ID3D11Texture2D* source = this->m_scene;

    if (this->m_sceneSamples > 1) {
        if (!this->m_sceneResolve) {
            D3D11_TEXTURE2D_DESC desc;
            this->m_scene->GetDesc(&desc);
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(device->CreateTexture2D(&desc, nullptr, &this->m_sceneResolve))) {
                this->m_sceneResolve = nullptr;
                return nullptr;
            }
        }

        this->m_device->m_d3dContext->ResolveSubresource(this->m_sceneResolve, 0, this->m_scene, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        source = this->m_sceneResolve;
    }

    if (!this->m_sceneSrv && FAILED(device->CreateShaderResourceView(source, nullptr, &this->m_sceneSrv))) {
        this->m_sceneSrv = nullptr;
    }

    return this->m_sceneSrv;
}

ID3D11BlendState* D3d11Lighting::IBlend(D3D11_BLEND src, D3D11_BLEND dst, uint8_t mask) {
    D3D11_BLEND_DESC desc;
    memset(&desc, 0, sizeof(desc));
    D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
    rt.BlendEnable = !(src == D3D11_BLEND_ONE && dst == D3D11_BLEND_ZERO);
    rt.SrcBlend = src;
    rt.DestBlend = dst;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = mask;
    return static_cast<ID3D11BlendState*>(this->m_device->IStateGet(State_Blend, &desc, sizeof(desc)));
}

void D3d11Lighting::IWriteConstants(uint32_t targetWidth, uint32_t targetHeight, const float* source) {
    PostConstants c;
    memset(&c, 0, sizeof(c));
    const GxLightingParams& p = this->m_params;
    memcpy(c.m_invProj, this->m_projInv, sizeof(c.m_invProj));
    memcpy(c.m_invView, this->m_viewInv, sizeof(c.m_invView));

    const D3D11_VIEWPORT& vp = this->m_viewport;
    c.m_viewport[0] = vp.TopLeftX;
    c.m_viewport[1] = vp.TopLeftY;
    c.m_viewport[2] = vp.Width > 0.0f ? vp.Width : 1.0f;
    c.m_viewport[3] = vp.Height > 0.0f ? vp.Height : 1.0f;
    c.m_depthRange[0] = vp.MinDepth;
    c.m_depthRange[1] = vp.MaxDepth;
    c.m_depthRange[2] = vp.MaxDepth > vp.MinDepth ? 1.0f / (vp.MaxDepth - vp.MinDepth) : 1.0f;
    c.m_target[0] = static_cast<float>(targetWidth);
    c.m_target[1] = static_cast<float>(targetHeight);
    c.m_target[2] = targetWidth ? 1.0f / targetWidth : 0.0f;
    c.m_target[3] = targetHeight ? 1.0f / targetHeight : 0.0f;

    c.m_ao[0] = p.m_ssaoRadius;
    c.m_ao[1] = p.m_ssaoIntensity;
    c.m_ao[2] = p.m_ssaoDistance * 0.6f;
    c.m_ao[3] = p.m_ssaoDistance;
    // pixels per yard at clip w 1; the 24-bit depth step in clip w per w squared (one step / the near plane)
    c.m_aoProj[0] = 0.5f * c.m_viewport[3] * fabsf(this->m_proj[5]);
    float nearPlane = this->m_proj[10] != 0.0f ? -this->m_proj[14] / this->m_proj[10] : 0.0f;
    c.m_aoProj[1] = nearPlane > 0.0f ? c.m_depthRange[2] / (16777216.0f * nearPlane) : 0.0f;

    c.m_fogColor[0] = p.m_fogColor.x;
    c.m_fogColor[1] = p.m_fogColor.y;
    c.m_fogColor[2] = p.m_fogColor.z;
    c.m_fogColor[3] = (p.m_features & GxLighting_Scattering) ? p.m_scattering : 0.0f;
    c.m_fogParams[0] = p.m_fogStart;
    c.m_fogParams[1] = p.m_fogEnd;
    c.m_fogParams[2] = p.m_fogDensity;
    c.m_fogParams[3] = p.m_fogEnd > p.m_fogStart ? 1.0f : 0.0f;
    c.m_sunColor[0] = p.m_sunColor.x;
    c.m_sunColor[1] = p.m_sunColor.y;
    c.m_sunColor[2] = p.m_sunColor.z;
    c.m_sunDir[0] = -p.m_sunDirection.x;
    c.m_sunDir[1] = -p.m_sunDirection.y;
    c.m_sunDir[2] = -p.m_sunDirection.z;
    c.m_heightFog[0] = (p.m_features & GxLighting_HeightFog) ? p.m_heightFogDensity : 0.0f;
    c.m_heightFog[1] = p.m_heightFogFalloff;
    c.m_heightFog[2] = p.m_heightFogBase;
    c.m_heightFog[3] = p.m_heightFogMax;
    c.m_camera[0] = p.m_cameraPosition.x;
    c.m_camera[1] = p.m_cameraPosition.y;
    c.m_camera[2] = p.m_cameraPosition.z;
    c.m_camera[3] = p.m_time;

    c.m_bloom[0] = p.m_bloomThreshold;
    c.m_bloom[1] = p.m_bloomThreshold * 0.5f;
    c.m_bloom[2] = p.m_bloomIntensity;
    c.m_bloom[3] = (p.m_features & GxLighting_Bloom) && p.m_bloomIntensity > 0.0f ? 1.0f : 0.0f;
    c.m_tone[0] = p.m_exposure;
    c.m_tone[1] = static_cast<float>(p.m_tonemap);
    c.m_tone[2] = static_cast<float>(p.m_debug);

    if (source) {
        memcpy(c.m_source, source, sizeof(c.m_source));
    }

    c.m_projW[0] = this->m_proj[3];
    c.m_projW[1] = this->m_proj[7];
    c.m_projW[2] = this->m_proj[11];
    c.m_projW[3] = this->m_proj[15];
    c.m_ssilvb[0] = p.m_ssaoIntensity;
    c.m_ssilvb[1] = p.m_ssgiIntensity;
    c.m_ssilvb[2] = p.m_ssilvbRadius;
    c.m_ssilvb[3] = p.m_ssilvbThickness;
    memcpy(c.m_skyRow, p.m_skyRow, sizeof(c.m_skyRow));
    memcpy(c.m_sky, p.m_skyParams, sizeof(c.m_sky));
    c.m_wet[0] = p.m_wetness;
    c.m_wet[1] = p.m_wetReflections ? 1.0f : 0.0f;
    c.m_wet[2] = this->m_wetShelter ? 1.0f : 0.0f;
    memcpy(c.m_origin, p.m_renderOrigin, sizeof(p.m_renderOrigin));
    memcpy(c.m_projFwd, this->m_proj, sizeof(c.m_projFwd));
    UploadConstants(this->m_device->m_d3dContext, this->m_constants, &c, sizeof(c));
}

void D3d11Lighting::IDraw(EShader shader, ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport, ID3D11BlendState* blend, ID3D11ShaderResourceView* const* srvs, uint32_t count) {
    CGxDeviceD3d11* dev = this->m_device;
    ID3D11DeviceContext* ctx = dev->m_d3dContext;

    D3D11_RASTERIZER_DESC rs;
    memset(&rs, 0, sizeof(rs));
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC ds;
    memset(&ds, 0, sizeof(ds));
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* sampler = static_cast<ID3D11SamplerState*>(dev->IStateGet(State_Sampler, &sd, sizeof(sd)));

    // the target first: a texture bound for writing unbinds itself as a source
    ID3D11ShaderResourceView* none[16] = {};
    ctx->PSSetShaderResources(0, 16, none);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(dev->IStateGet(State_Depth, &ds, sizeof(ds))), 0);
    ctx->RSSetState(static_cast<ID3D11RasterizerState*>(dev->IStateGet(State_Raster, &rs, sizeof(rs))));
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(static_cast<ID3D11VertexShader*>(this->m_shaders[Shader_FullscreenVs]), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(static_cast<ID3D11PixelShader*>(this->m_shaders[shader]), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &this->m_constants);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->PSSetShaderResources(0, count, srvs);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 16, none);
}

// as IBlit's tail: the device's targets back, and everything the next draw needs resent
void D3d11Lighting::IRestoreDevice() {
    CGxDeviceD3d11* dev = this->m_device;
    dev->IOMSetTargets();
    memset(dev->m_curSrv, 0, sizeof(dev->m_curSrv));
    memset(dev->m_curCb, 0, sizeof(dev->m_curCb));
    dev->m_curVs = nullptr;
    dev->m_curPs = nullptr;
    dev->m_curLayoutKey = 0;
    dev->m_curTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    dev->m_stateDirty |= Dirty_Raster | Dirty_Blend | Dirty_Depth;
    dev->m_samplerDirty |= 1;
    dev->m_viewportDirty = 1;
    dev->m_scissorRectDirty = 1;
}

void D3d11Lighting::BeginWorld(const GxLightingParams& params) {
    CGxDeviceD3d11* dev = this->m_device;

    if (this->m_worldActive) {
        this->EndWorld();
    }

    if (!dev->m_context || !dev->m_backBuffer || !this->ICreate()) {
        return;
    }

    this->m_params = params;
    this->m_worldActive = true;
    this->m_viewSet = false;

    // the world viewport: CGWorldFrame::OnWorldRender has just set it, on the frame
    dev->IXformSetViewport();
    UINT count = 1;
    dev->m_d3dContext->RSGetViewports(&count, &this->m_viewport);

    if (!(params.m_features & GxLighting_Hdr) || dev->m_textureTarget[GxBuffers_Color].m_texture) {
        return;
    }

    D3D11_TEXTURE2D_DESC bb;
    dev->m_backBuffer->GetDesc(&bb);

    if (!this->IEnsureScene(bb.Width, bb.Height, bb.SampleDesc.Count)) {
        return;
    }

    // nothing outside the world's viewport is drawn into it: black, so the bloom finds nothing there
    float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dev->m_d3dContext->ClearRenderTargetView(this->m_sceneRtv, clear);
    this->m_sceneResolveValid = false;
    this->ISwap();
    dev->IBindRenderTargets();
}

void D3d11Lighting::SetupView(const GxLightingParams& params) {
    if (!this->m_worldActive) {
        return;
    }

    CGxDeviceD3d11* dev = this->m_device;
    this->m_params = params;
    const CGxMatrixStack& viewStack = dev->m_xforms[GxXform_View];
    const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
    C44Matrix viewInv = view.Inverse();
    C44Matrix projInv = dev->m_projNative.Inverse();
    memcpy(this->m_viewInv, &viewInv, sizeof(this->m_viewInv));
    memcpy(this->m_proj, &dev->m_projNative, sizeof(this->m_proj));
    memcpy(this->m_projInv, &projInv, sizeof(this->m_projInv));
    this->m_viewSet = true;
}

void D3d11Lighting::OpaqueDone() {
    CGxDeviceD3d11* dev = this->m_device;
    const GxLightingParams& p = this->m_params;
    bool ssao = (p.m_features & GxLighting_Ssao) && (p.m_ssaoIntensity > 0.0f || (p.m_ssilvb && p.m_ssgiIntensity > 0.0f));
    bool fog = (p.m_features & (GxLighting_HeightFog | GxLighting_Scattering)) != 0;
    bool sky = (p.m_features & GxLighting_SkyOcclusion) && p.m_skyHeights;
    bool wet = (p.m_features & GxLighting_Wet) && p.m_wetness > 0.0f;

    if (!this->m_worldActive || !this->m_viewSet || (!ssao && !fog && !sky && !wet) || !dev->m_depthSrv) {
        return;
    }

    // the world is drawing into a texture of its own (or the depth is not the frame's): nothing to do here
    if (!dev->m_backBufferRtv || dev->m_curRtv != dev->m_backBufferRtv || dev->m_curDsv != dev->m_depthDsv) {
        return;
    }

    D3D11_TEXTURE2D_DESC depthDesc;
    dev->m_depthTexture->GetDesc(&depthDesc);
    bool msaa = depthDesc.SampleDesc.Count > 1;

    if (msaa && !this->m_shaders[Shader_AoPsMs]) {
        return;
    }

    D3D11_TEXTURE2D_DESC bb;
    dev->m_backBuffer->GetDesc(&bb);
    ID3D11RenderTargetView* frame = dev->m_backBufferRtv;
    ID3D11ShaderResourceView* depth = dev->m_depthSrv;
    const D3D11_VIEWPORT& vp = this->m_viewport;
    // colour only: the frame's alpha is FFX glow's mask
    const uint8_t rgb = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    if (ssao && p.m_ssilvb) {
        this->ISsilvb(depth, msaa);
    } else if (ssao) {
        uint32_t width = static_cast<uint32_t>(vp.Width + 1.0f) / 2;
        uint32_t height = static_cast<uint32_t>(vp.Height + 1.0f) / 2;

        if (this->IEnsureTarget(this->m_ao[0], width, height, DXGI_FORMAT_R16G16_FLOAT) && this->IEnsureTarget(this->m_ao[1], width, height, DXGI_FORMAT_R16G16_FLOAT)) {
            D3D11_VIEWPORT half = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
            float source[4] = { 1.0f / width, 1.0f / height, 0.0f, 0.0f };
            ID3D11BlendState* opaque = this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL);

            this->IWriteConstants(width, height, source);
            this->IDraw(msaa ? Shader_AoPsMs : Shader_AoPs, this->m_ao[0].m_rtv, half, opaque, &depth, 1);

            ID3D11ShaderResourceView* blurH[2] = { nullptr, this->m_ao[0].m_srv };
            source[2] = 1.0f;
            this->IWriteConstants(width, height, source);
            this->IDraw(Shader_BlurPs, this->m_ao[1].m_rtv, half, opaque, blurH, 2);

            ID3D11ShaderResourceView* blurV[2] = { nullptr, this->m_ao[1].m_srv };
            source[2] = 0.0f;
            source[3] = 1.0f;
            this->IWriteConstants(width, height, source);
            this->IDraw(Shader_BlurPs, this->m_ao[0].m_rtv, half, opaque, blurV, 2);

            ID3D11ShaderResourceView* composite[2] = { depth, this->m_ao[0].m_srv };
            source[3] = 0.0f;
            this->IWriteConstants(bb.Width, bb.Height, source);
            // lightingDebug 1, 4, 5: the occlusion (or its inputs) instead of the scene
            ID3D11BlendState* blend = p.m_debug == 1 || p.m_debug >= 4 ? this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, rgb) : this->IBlend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, rgb);
            this->IDraw(msaa ? Shader_AoCompositePsMs : Shader_AoCompositePs, frame, vp, blend, composite, 2);
        }
    }

    // BENILLA sky occlusion: x mix(floor, 1, open sky) into the frame (lightingDebug 6: the factor alone)
    if (sky && this->ISkyMap()) {
        this->IWriteConstants(bb.Width, bb.Height, nullptr);
        ID3D11BlendState* blend = p.m_debug == 6 ? this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, rgb) : this->IBlend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, rgb);
        ID3D11ShaderResourceView* views[4] = { depth, nullptr, nullptr, this->m_skySrv };
        this->IDraw(msaa ? Shader_SkyOcclusionPsMs : Shader_SkyOcclusionPs, frame, vp, blend, views, 4);
    }

    if (wet) {
        this->IWet(depth, msaa, p.m_skyHeights && this->ISkyMap());
    }

    if (fog) {
        this->IWriteConstants(bb.Width, bb.Height, nullptr);
        this->IDraw(msaa ? Shader_FogPsMs : Shader_FogPs, frame, vp, this->IBlend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, rgb), &depth, 1);
    }

    dev->m_resolveValid = false;
    this->IRestoreDevice();
}

void D3d11Lighting::EndWorld() {
    CGxDeviceD3d11* dev = this->m_device;

    if (!this->m_worldActive) {
        return;
    }

    this->m_worldActive = false;

    if (!this->m_swapped) {
        return;
    }

    // the world is done: the back buffer is the target again, and the scene goes into it tonemapped
    this->ISwap();
    dev->m_resolveValid = false;
    dev->IBindRenderTargets();
    ID3D11ShaderResourceView* scene = this->ISceneSrv();

    if (!scene || !dev->m_backBufferRtv) {
        this->IRestoreDevice();
        return;
    }

    const GxLightingParams& p = this->m_params;
    D3D11_TEXTURE2D_DESC bb;
    dev->m_backBuffer->GetDesc(&bb);
    D3D11_VIEWPORT vp = this->m_viewport;

    if (vp.Width < 1.0f || vp.Height < 1.0f) {
        vp = { 0.0f, 0.0f, static_cast<float>(bb.Width), static_cast<float>(bb.Height), 0.0f, 1.0f };
    }

    // the lift multiplies the exposure the bloom and the tonemap both use
    this->IAutoExposure(scene, bb.Width, bb.Height);
    ID3D11BlendState* opaque = this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL);
    uint32_t levels = 0;

    if ((p.m_features & GxLighting_Bloom) && p.m_bloomIntensity > 0.0f) {
        uint32_t width = bb.Width;
        uint32_t height = bb.Height;

        for (; levels < s_bloomLevels; levels++) {
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;

            if (!this->IEnsureTarget(this->m_bloom[levels], width, height, DXGI_FORMAT_R11G11B10_FLOAT)) {
                break;
            }
        }
    }

    if (levels) {
        // down: the bright part of the scene to half size, then halved again level by level
        float source[4] = { 1.0f / bb.Width, 1.0f / bb.Height, 0.0f, 0.0f };
        ID3D11ShaderResourceView* views[2] = { nullptr, scene };
        Target& first = this->m_bloom[0];
        D3D11_VIEWPORT level = { 0.0f, 0.0f, static_cast<float>(first.m_width), static_cast<float>(first.m_height), 0.0f, 1.0f };
        this->IWriteConstants(first.m_width, first.m_height, source);
        this->IDraw(Shader_BloomPrefilterPs, first.m_rtv, level, opaque, views, 2);

        for (uint32_t i = 1; i < levels; i++) {
            Target& from = this->m_bloom[i - 1];
            Target& to = this->m_bloom[i];
            source[0] = 1.0f / from.m_width;
            source[1] = 1.0f / from.m_height;
            views[1] = from.m_srv;
            level = { 0.0f, 0.0f, static_cast<float>(to.m_width), static_cast<float>(to.m_height), 0.0f, 1.0f };
            this->IWriteConstants(to.m_width, to.m_height, source);
            this->IDraw(Shader_BloomDownPs, to.m_rtv, level, opaque, views, 2);
        }

        // up: each level spread and added into the one above
        ID3D11BlendState* add = this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_COLOR_WRITE_ENABLE_ALL);

        for (uint32_t i = levels - 1; i > 0; i--) {
            Target& from = this->m_bloom[i];
            Target& to = this->m_bloom[i - 1];
            source[0] = 1.0f / from.m_width;
            source[1] = 1.0f / from.m_height;
            views[1] = from.m_srv;
            level = { 0.0f, 0.0f, static_cast<float>(to.m_width), static_cast<float>(to.m_height), 0.0f, 1.0f };
            this->IWriteConstants(to.m_width, to.m_height, source);
            this->IDraw(Shader_BloomUpPs, to.m_rtv, level, add, views, 2);
        }
    }

    // the tonemap over the world viewport of the frame; z the bloom levels summed
    float source[4] = { 1.0f / bb.Width, 1.0f / bb.Height, static_cast<float>(levels ? levels : 1), 0.0f };
    uint32_t features = this->m_params.m_features;

    if (!levels) {
        this->m_params.m_features &= ~GxLighting_Bloom;
    }

    this->IWriteConstants(bb.Width, bb.Height, source);
    this->m_params.m_features = features;
    ID3D11ShaderResourceView* views[3] = { nullptr, scene, levels ? this->m_bloom[0].m_srv : nullptr };
    this->IDraw(Shader_TonemapPs, dev->m_backBufferRtv, vp, opaque, views, 3);
    dev->m_resolveValid = false;
    this->IRestoreDevice();
}

// BENILLA SSILVB (benilla-bourbon ssilvb.rs): gather at full resolution over the world viewport, blur H then V,
// multiply into the frame. The indirect half reads the frame's colour (resolved when multisampled).
void D3d11Lighting::ISsilvb(ID3D11ShaderResourceView* depth, bool msaa) {
    CGxDeviceD3d11* dev = this->m_device;
    const GxLightingParams& p = this->m_params;
    const D3D11_VIEWPORT& vp = this->m_viewport;
    uint32_t width = static_cast<uint32_t>(vp.Width);
    uint32_t height = static_cast<uint32_t>(vp.Height);

    if (!this->IEnsureTarget(this->m_ssilvb[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) || !this->IEnsureTarget(this->m_ssilvb[1], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        return;
    }

    ID3D11ShaderResourceView* color = nullptr;
    bool gi = p.m_ssgiIntensity > 0.0f;

    if (gi) {
        ID3D11Texture2D* source = dev->IResolvedBackBuffer();

        if (source && !dev->m_backBufferSrv && FAILED(dev->m_d3dDevice->CreateShaderResourceView(source, nullptr, &dev->m_backBufferSrv))) {
            dev->m_backBufferSrv = nullptr;
        }

        color = source ? dev->m_backBufferSrv : nullptr;
        gi = color != nullptr;
    }

    EShader gather = gi ? (msaa ? Shader_SsilvbPsMs : Shader_SsilvbPs) : (msaa ? Shader_SsilvbAoPsMs : Shader_SsilvbAoPs);
    EShader blur = msaa ? Shader_SsilvbBlurPsMs : Shader_SsilvbBlurPs;
    D3D11_VIEWPORT full = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    float source[4] = { 1.0f / width, 1.0f / height, 0.0f, 0.0f };
    ID3D11BlendState* opaque = this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL);
    D3D11_TEXTURE2D_DESC bb;
    dev->m_backBuffer->GetDesc(&bb);

    this->IWriteConstants(width, height, source);
    ID3D11ShaderResourceView* gatherViews[2] = { depth, color };
    this->IDraw(gather, this->m_ssilvb[0].m_rtv, full, opaque, gatherViews, 2);

    source[2] = 1.0f;
    this->IWriteConstants(width, height, source);
    ID3D11ShaderResourceView* blurH[2] = { depth, this->m_ssilvb[0].m_srv };
    this->IDraw(blur, this->m_ssilvb[1].m_rtv, full, opaque, blurH, 2);

    source[2] = 0.0f;
    source[3] = 1.0f;
    this->IWriteConstants(width, height, source);
    ID3D11ShaderResourceView* blurV[2] = { depth, this->m_ssilvb[1].m_srv };
    this->IDraw(blur, this->m_ssilvb[0].m_rtv, full, opaque, blurV, 2);

    // lightingDebug 1, 4, 5: the factor instead of the scene
    const uint8_t rgb = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ID3D11BlendState* blend = p.m_debug == 1 || p.m_debug >= 4 ? this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, rgb) : this->IBlend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, rgb);
    this->IWriteConstants(bb.Width, bb.Height, nullptr);
    ID3D11ShaderResourceView* composite[2] = { nullptr, this->m_ssilvb[0].m_srv };
    this->IDraw(Shader_SsilvbCompositePs, dev->m_backBufferRtv, vp, blend, composite, 2);
}

// BENILLA sky occlusion: (re)create the heights texture at the map's size, upload when the serial moved.
bool D3d11Lighting::ISkyMap() {
    const GxLightingParams& p = this->m_params;
    const uint32_t size = static_cast<uint32_t>(p.m_skyRow[3]);
    if (!size || !p.m_skyHeights) {
        return false;
    }
    if (this->m_skySize != size) {
        SafeRelease(this->m_skySrv);
        SafeRelease(this->m_skyTex);
        this->m_skySize = 0;
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof(d));
        d.Width = size;
        d.Height = size;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { p.m_skyHeights, size * 4, 0 };
        if (FAILED(this->m_device->m_d3dDevice->CreateTexture2D(&d, &init, &this->m_skyTex)) ||
            FAILED(this->m_device->m_d3dDevice->CreateShaderResourceView(this->m_skyTex, nullptr, &this->m_skySrv))) {
            SafeRelease(this->m_skyTex);
            return false;
        }
        this->m_skySize = size;
        this->m_skySerial = p.m_skySerial;
    } else if (this->m_skySerial != p.m_skySerial) {
        this->m_device->m_d3dContext->UpdateSubresource(this->m_skyTex, 0, nullptr, p.m_skyHeights, size * 4, 0);
        this->m_skySerial = p.m_skySerial;
    }
    return true;
}

// BENILLA rain on surfaces: snapshot the opaque frame, multiply it by the wet albedo's ratio, add the reflection.
void D3d11Lighting::IWet(ID3D11ShaderResourceView* depth, bool msaa, bool shelter) {
    CGxDeviceD3d11* dev = this->m_device;
    const GxLightingParams& p = this->m_params;
    D3D11_TEXTURE2D_DESC bb;
    dev->m_backBuffer->GetDesc(&bb);
    D3D11_TEXTURE2D_DESC have = {};

    if (this->m_wetTex) {
        this->m_wetTex->GetDesc(&have);
    }

    if (!this->m_wetTex || have.Width != bb.Width || have.Height != bb.Height || have.Format != bb.Format) {
        SafeRelease(this->m_wetSrv);
        SafeRelease(this->m_wetTex);
        D3D11_TEXTURE2D_DESC d = bb;
        d.MipLevels = 3;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags = 0;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->m_d3dDevice->CreateTexture2D(&d, nullptr, &this->m_wetTex)) ||
            FAILED(dev->m_d3dDevice->CreateShaderResourceView(this->m_wetTex, nullptr, &this->m_wetSrv))) {
            SafeRelease(this->m_wetTex);
            return;
        }
    }

    ID3D11DeviceContext* ctx = dev->m_d3dContext;
    if (msaa) {
        ctx->ResolveSubresource(this->m_wetTex, 0, dev->m_backBuffer, 0, bb.Format);
    } else {
        ctx->CopySubresourceRegion(this->m_wetTex, 0, 0, 0, 0, dev->m_backBuffer, 0, nullptr);
    }
    ctx->GenerateMips(this->m_wetSrv);

    const uint8_t rgb = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    float source[4] = { 1.0f / bb.Width, 1.0f / bb.Height, 0.0f, 0.0f };
    this->m_wetShelter = shelter;
    this->IWriteConstants(bb.Width, bb.Height, source);
    ID3D11ShaderResourceView* views[4] = { depth, this->m_wetSrv, this->m_wetSrv, shelter ? this->m_skySrv : nullptr };
    // lightingDebug 7: the wetness terms instead of the scene
    ID3D11BlendState* darken = p.m_debug == 7 ? this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, rgb) : this->IBlend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, rgb);
    this->IDraw(msaa ? Shader_WetDarkenPsMs : Shader_WetDarkenPs, dev->m_backBufferRtv, this->m_viewport, darken, views, 4);

    if (p.m_debug != 7) {
        this->IDraw(msaa ? Shader_WetReflectPsMs : Shader_WetReflectPs, dev->m_backBufferRtv, this->m_viewport, this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, rgb), views, 4);
    }
}

// BENILLA auto-exposure: read back the last meter grid if it has landed (never waiting), meter this frame's scene when
// none is in flight, and ease the lift toward what the scene key asks for. Scales m_params.m_exposure.
void D3d11Lighting::IAutoExposure(ID3D11ShaderResourceView* scene, uint32_t width, uint32_t height) {
    GxLightingParams& p = this->m_params;
    ID3D11DeviceContext* ctx = this->m_device->m_d3dContext;
    float dt = this->m_lastTime < 0.0f ? 0.0f : p.m_time - this->m_lastTime;
    dt = dt < 0.0f || dt > 0.25f ? 0.0f : dt;   // m_time wraps hourly; a hitch does not snap the eye
    this->m_lastTime = p.m_time;
    this->m_sceneKeyAge += dt;
    bool on = p.m_autoExposureMax > 1.0f;

    if (this->m_meterPending) {
        D3D11_MAPPED_SUBRESOURCE mapped;

        if (SUCCEEDED(ctx->Map(this->m_meterStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            float sum = 0.0f;

            for (uint32_t y = 0; y < s_meterHeight; y++) {
                const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);

                for (uint32_t x = 0; x < s_meterWidth; x++) {
                    sum += row[x];
                }
            }

            ctx->Unmap(this->m_meterStaging, 0);
            this->m_meterPending = false;
            float mean = sum / (s_meterWidth * s_meterHeight);

            if (std::isfinite(mean)) {
                this->m_sceneKey = exp2f(mean);
                this->m_sceneKeyAge = 0.0f;
            }
        }
    }

    if (on && !this->m_meterPending && this->m_shaders[Shader_MeterPs] && this->IEnsureTarget(this->m_meter, s_meterWidth, s_meterHeight, DXGI_FORMAT_R32_FLOAT)) {
        if (!this->m_meterStaging) {
            D3D11_TEXTURE2D_DESC desc;
            this->m_meter.m_texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            if (FAILED(this->m_device->m_d3dDevice->CreateTexture2D(&desc, nullptr, &this->m_meterStaging))) {
                this->m_meterStaging = nullptr;
            }
        }

        if (this->m_meterStaging) {
            float source[4] = { 1.0f / width, 1.0f / height, 0.0f, 0.0f };
            D3D11_VIEWPORT grid = { 0.0f, 0.0f, static_cast<float>(s_meterWidth), static_cast<float>(s_meterHeight), 0.0f, 1.0f };
            ID3D11ShaderResourceView* views[2] = { nullptr, scene };
            this->IWriteConstants(s_meterWidth, s_meterHeight, source);
            this->IDraw(Shader_MeterPs, this->m_meter.m_rtv, grid, this->IBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_COLOR_WRITE_ENABLE_ALL), views, 2);
            ctx->CopyResource(this->m_meterStaging, this->m_meter.m_texture);
            this->m_meterPending = true;
        }
    }

    // the lift a metered scene asks for under the look's own exposure: 1 at the target and above, rising toward the
    // ceiling as the picture darkens (benilla screen_lift); off eases back to 1; a stale meter holds the eye
    float lift = this->m_eye;

    if (!on) {
        lift = 1.0f;
    } else if (this->m_sceneKeyAge < s_meterStaleSeconds) {
        float key = this->m_sceneKey * p.m_exposure;
        lift = powf(p.m_autoExposureTarget / (key > 1e-5f ? key : 1e-5f), s_autoExposureStrength);
        lift = lift < 1.0f ? 1.0f : (lift > p.m_autoExposureMax ? p.m_autoExposureMax : lift);

        // the first reading is taken whole: entering the world at night does not open on the eye adjusting
        if (!this->m_eyeSet) {
            this->m_eye = lift;
            this->m_eyeSet = true;
        }
    }

    this->m_eye += (lift - this->m_eye) * (1.0f - expf(-dt / s_autoExposureSeconds));
    p.m_exposure *= this->m_eye;
}

void D3d11Lighting::ReleaseTargets() {
    // a resize or a lost device in the middle of the world: the back buffer goes back first
    if (this->m_swapped) {
        this->ISwap();
    }

    this->m_worldActive = false;
    SafeRelease(this->m_sceneSrv);
    SafeRelease(this->m_sceneResolve);
    SafeRelease(this->m_sceneRtv);
    SafeRelease(this->m_scene);
    this->m_sceneWidth = 0;
    this->m_sceneHeight = 0;
    this->m_sceneSamples = 0;

    for (Target& target : this->m_ao) {
        ReleaseTarget(target);
    }

    for (Target& target : this->m_bloom) {
        ReleaseTarget(target);
    }

    for (Target& target : this->m_ssilvb) {
        ReleaseTarget(target);
    }

    ReleaseTarget(this->m_meter);
    SafeRelease(this->m_skySrv);
    SafeRelease(this->m_skyTex);
    this->m_skySize = 0;
    SafeRelease(this->m_wetSrv);
    SafeRelease(this->m_wetTex);
    SafeRelease(this->m_meterStaging);
    this->m_meterPending = false;
}

void D3d11Lighting::ReleaseAll() {
    this->ReleaseTargets();

    for (ID3D11DeviceChild*& shader : this->m_shaders) {
        SafeRelease(shader);
    }

    SafeRelease(this->m_constants);
    this->m_failed = false;
}

}

void D3d11LightingCreate(CGxDeviceD3d11* device) {
    if (s_lighting) {
        s_lighting->ReleaseAll();
        delete s_lighting;
    }

    s_lighting = new D3d11Lighting(device);
    g_gxLighting = s_lighting;

    SafeRelease(s_gpuConstants);
    static const GxLightingGpu s_zero = {};
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(GxLightingGpu);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA init = { &s_zero, 0, 0 };

    if (FAILED(device->m_d3dDevice->CreateBuffer(&desc, &init, &s_gpuConstants))) {
        s_gpuConstants = nullptr;
        D3d11Log("D3d11Lighting: no GxLighting cbuffer; the lit shaders stay on their stock path");
    }

    // the first draw uploads whatever the game has (a recreated device starts zeroed)
    s_gpuSerial = g_gxLightingGpuSerial - 1;
}

void D3d11LightingSyncShaderConstants() {
    if (!s_gpuConstants || s_gpuSerial == g_gxLightingGpuSerial) {
        return;
    }

    s_gpuSerial = g_gxLightingGpuSerial;
    ID3D11DeviceContext* ctx = s_lighting->Context();
    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(ctx->Map(s_gpuConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &g_gxLightingGpu, sizeof(GxLightingGpu));
        ctx->Unmap(s_gpuConstants, 0);
    }
}

void D3d11LightingBindShaderConstants() {
    if (s_gpuConstants) {
        ID3D11DeviceContext* ctx = s_lighting->Context();
        ctx->VSSetConstantBuffers(3, 1, &s_gpuConstants);
        ctx->PSSetConstantBuffers(3, 1, &s_gpuConstants);
    }
}

void D3d11LightingDestroy() {
    SafeRelease(s_gpuConstants);

    if (s_lighting) {
        s_lighting->ReleaseAll();
    }

    // the object stays until the next D3d11LightingCreate; ReleaseAll dropped every D3D object
    g_gxLighting = nullptr;
}

void D3d11LightingReleaseTargets() {
    if (s_lighting) {
        s_lighting->ReleaseTargets();
    }
}
