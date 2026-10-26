// foreverFog obeys the standard vfog sliders (PLAN item 18): at the slider defaults the Forever layers are unchanged,
// vfogSunScatter scales every layer's in-scatter, vfogShaftIntensity the off-sun share, vfogDensity the density.
// Build: g++ -std=c++17 -I/root/wowdecomp forever_slider_check.cpp \
//   /root/wowdecomp/WoW/Source/WorldClient/VolumetricFogModel.cpp -o /tmp/fsc && /tmp/fsc   (exit 0 = pass)
// With -DOLD and the HEAD~ model in place of the new one it prints the old defaults for a diff.
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace VolumetricFog;
// The fogdata.bin lookup lives in VolumetricFogData.cpp (file IO); foreverFog never resolves it.
bool VolumetricFog::ResolveFogData(int, const float*, float, const LightParamsSelection&, AuthoredFog&) { return false; }

static FrameInputs Dawn() {
    FrameInputs in = {};
    in.foreverFog = true;
    in.look = ForeverNeutralLook();
    in.fogColor = 0xffb08a70u; in.ambientColor = 0xff404858u; in.directColor = 0xffffd0a0u; in.sunColor = 0xffffe0b0u;
    in.toLight[0] = 0.9f; in.toLight[1] = 0.0f; in.toLight[2] = 0.2f;
    in.fogStart = 300.0f; in.fogEnd = 900.0f; in.zoneFogDistance = 4000.0f; in.farClip = 1000.0f; in.fogCap = 2500.0f;
    in.camPos[2] = 60.0f;
    return in;
}

int main() {
    const FrameInputs in = Dawn();
    const Config base;
    const FogParams p0 = BuildFogParamsFrom(in, base, nullptr);
    for (int j = 0; j < kFogLayers; ++j)
        std::printf("layer %d density %.6g iso %.6g diffuse %.6g %.6g %.6g emissive %.6g\n", j, p0.layers[j].density,
                    p0.layers[j].isotropic, p0.layers[j].diffuse[0], p0.layers[j].diffuse[1], p0.layers[j].diffuse[2], p0.layers[j].emissive[0]);
#ifndef OLD
    Config c = base; c.sunScatter = base.sunScatter * 2.0f;
    const FogParams p1 = BuildFogParamsFrom(in, c, nullptr);
    c = base; c.shaftIntensity = 0.0f; c.shafts = false;
    const FogParams p2 = BuildFogParamsFrom(in, c, nullptr);
    c = base; c.density = 2.0f;
    const FogParams p3 = BuildFogParamsFrom(in, c, nullptr);
    for (int j = 0; j < kFogLayers; ++j) {
        for (int k = 0; k < 3; ++k)
            assert(std::fabs(p1.layers[j].diffuse[k] - 2.0f * p0.layers[j].diffuse[k]) <= 1e-5f * (1.0f + p0.layers[j].diffuse[k]));
        if (p0.layers[j].isotropic < 0.0f) assert(std::fabs(p2.layers[j].isotropic) <= 1e-6f);
        assert(std::fabs(p3.layers[j].density - 2.0f * p0.layers[j].density) <= 1e-6f * (1.0f + p0.layers[j].density));
    }
    std::puts("forever slider check: pass");
#endif
    return 0;
}
