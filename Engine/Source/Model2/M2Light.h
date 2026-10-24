#ifndef ENGINE_SOURCE_MODEL2_M2LIGHT_H
#define ENGINE_SOURCE_MODEL2_M2LIGHT_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C4Plane.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "Engine/Source/Model2/Types.h"

class CM2Light;
class CM2Scene;
class CM2Model;

class CM2Light {
    public:
    /* 0000 */ CM2Scene* m_scene;
    /* 0004 */ uint32_t m_stamp;
    /* 0008 */ M2LIGHTTYPE m_type;
    /* 000C */ C3Vector m_pos;
    /* 0018 */ C3Vector m_viewPos;
    /* 0024 */ C3Vector m_dir;
    /* 0030 */ C3Vector m_ambColor;
    /* 003C */ C3Vector m_dirColor;
    /* 0048 */ C3Vector m_specColor;
    /* 0054 */ float m_constantAttenuation;
    /* 0058 */ float m_linearAttenuation;
    /* 005C */ float m_quadraticAttenuation;
    /* 0060 */ uint32_t m_visible;
    /* 0064 */ CM2Light** m_lightPrev;
    /* 0068 */ CM2Light* m_lightNext;

    CM2Light();
    ~CM2Light();
    void GxLightSet(uint32_t index);
    void Initialize(CM2Scene* scene);
    void Link();
    void SetDirection(const C3Vector& dir);
    void SetLightType(M2LIGHTTYPE type);
    void SetPosition(const C3Vector& pos);
    void SetVisible(int32_t visible);
    void Unlink();
};

class CM2Lighting {
    public:
    /* 0000 */ CM2Scene* m_scene;
    /* 0004 */ CAaSphere m_sphere4;
    /* 0014 */ uint32_t m_flags;
    /* 0018 */ C3Vector m_vector18;
    /* 0024 */ C3Vector m_vector24;
    /* 0030 */ C3Vector m_vector30;
    /* 003C */ C3Vector m_vector3C;
    /* 0048 */ C3Vector m_vector48;
    /* 0054 */ C3Vector m_sunAmbient;
    /* 0060 */ C3Vector m_sunDiffuse;
    /* 006C */ C3Vector m_sunSpecular;
    /* 0078 */ C3Vector m_sunDir;
    /* 0084 */ CM2Light* m_lights [4];
    /* 0094 */ float m_lightDist [4];
    /* 00A4 */ uint32_t m_lightCount;
    /* 00A8 */ float m_fogStart;
    /* 00AC */ float m_fogEnd;
    /* 00B0 */ float m_fogScale;
    /* 00B4 */ float m_fogDensity;
    /* 00B8 */ C3Vector m_fogColor;
    /* 00C4 */ C4Plane m_liquidPlane;

    CM2Lighting();
    CM2Lighting(const CAaSphere& sphere);
    CM2Lighting(const C3Vector& position);
    void Reset();
    void AddAmbient(const C3Vector& color);
    void AddDiffuse(const C3Vector& color, const C3Vector& dir);
    void AddLight(CM2Light* light);
    void CameraSpace();
    int32_t GetPointLight(uint32_t index, C3Vector& pos, C3Vector& color, C3Vector& attenuation);
    int32_t GetSunLight(C3Vector& dir, C3Vector& ambient, C3Vector& diffuse, C3Vector& specular);
    void Initialize(CM2Scene* scene, const CAaSphere& sphere);
    void SetFog(const C3Vector& color, float start, float end);
    void SetFog(const C3Vector& color, float start, float end, float density);
    void SetupGxFog();
    void SetupGxLights(const C3Vector* origin);
    void SetupSunlight();
};

// LIGHTING (not in 12340): the scene whose point lights the shader lighting draws instead (WoW/Source/Ui/
// WorldLighting.cpp sets it on D3D11/Vulkan with shaderLighting, lightDynamic and lightReplaceStock on; else null).
// CM2Lighting::AddLight skips that scene's point lights; the sun, moon and ambient still pass.
extern CM2Scene* g_m2ShaderOwnedLightScene;
// UNITLIGHTS (not in 12340): a model that keeps that scene's stock per-vertex point lights anyway (WorldLighting.cpp:
// units and what they carry; the shader lighting's dynamic lights never reached them). Null = none keeps them.
// g_m2KeepPointLightsNow is set by CM2Model::SetupLighting while the model it asks about selects its lights.
extern bool (*g_m2KeepStockPointLights)(const CM2Model* model);
extern bool g_m2KeepPointLightsNow;
// POINTLIGHT (not in 12340): set while a WMO group or terrain chunk gathers its lights and the LegionLights pass
// (lightMode) lights them per pixel instead: CM2Lighting::AddLight then takes no point lights (the stock vertex
// shaders lit them per vertex, a patchwork of triangles under a torch)
extern bool g_m2SkipPointLights;
// LEGION lights (not in 12340): the scene whose models' point lights the LegionLights pass lights per pixel
// (lightMode 2); CM2Model::SetupLighting sets g_m2SkipPointLights for that scene's models
extern CM2Scene* g_m2PixelLitScene;

#endif
