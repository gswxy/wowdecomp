#ifndef WOW_SOURCE_WORLDCLIENT_SHADOWQUERY_H
#define WOW_SOURCE_WORLDCLIENT_SHADOWQUERY_H

#include <cstdint>
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Model2/M2Model.h"

struct CVar;

extern CVar* g_shadowCull;
extern CVar* g_shadowScissor;
extern CVar* g_shadowInstancing;

struct ShadowQueryBuffers {
    /* 0000 */ uint32_t m_groupCount;
    /* 0004 */ void* m_groups;
    /* 0008 */ void* m_groupData;
    /* 000C */ CModelBatchList m_untextured;
    /* 0018 */ CModelBatchList m_textured;
};

class CShadowQuery {
    public:
    static ShadowQueryBuffers s_bandBuffers[3];
    static float s_cutoffHeight;

    static void Update();
    static bool CameraIndoors();   // not stock: the camera counts as in a WMO interior (the steep stock light)
    static bool SunLatched(C3Vector& toward);   // not stock: gfxSunShadows' latched light, if it is in use
    static float WaterShadowStrength();       // not stock: shadowWaterStrength, 0-1

    static void FreeBuffers();
    static void AllocBuffers();
    static void GatherShadowCasters(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand);
    static void QueryMapObjDefs(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand);
    static void QueryTerrain(CShadowCacheQueryData& data, int32_t firstBand, int32_t lastBand);
    static void QueryMapObjDefsBand(CShadowCacheQueryData& data, int32_t band);
    static void QueryTerrainBand(CShadowCacheQueryData& data, int32_t band);
    static uint8_t s_stamp;
    static void Clear(CGxTex* target, CGxTex* depthTarget, float* rect);
    static void GetSize(CShadowCacheQueryData& data, int32_t band, float* minSize, float* maxSize);
    static void FrustumCallback(CFrustum& frustum, C44Matrix& matrix, const C3Vector& direction, CShadowCacheQueryData& data, int32_t band);
    static void Initialize();
    static void MatrixCallback(C3Vector& center, float radius, C44Matrix& matrix, C3Vector& direction, int32_t band);
    static int32_t QueryCallback(CShadowCacheQueryData& data, uint32_t frame);
    static int32_t RenderCallback(CShadowCacheQueryData& data, int32_t band, CGxTex* target, CGxTex* colorTarget, C3Vector& direction);
};

#endif
