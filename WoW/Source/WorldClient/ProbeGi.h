#pragma once

#include <cstdint>

// BENILLA probe GI stage 1 (benilla-bourbon modern/gi P5: gi_trace.wgsl trace_rays + sum_probes, probegi.wgsl
// pick/accum/finish; not in 12340): camera-centred cascades of irradiance probes (order-1 SH), traced on the CPU through
// the world's collision under a ms budget a frame, eased with benilla's hysteresis, sampled by a D3D11 post pass that
// swaps the stock ambient for the probes' irradiance (Lighting.hlsl GiPs).
namespace ProbeGi {

constexpr int kCascades = 3;
constexpr int kNx = 16, kNy = 16, kNz = 8;     // probes per axis (z up)
constexpr int kTexels = 5;                      // a probe: dc+valid, sh x+sky, sh y, sh z, distance moments
constexpr int kTexW = kNx * kTexels;
constexpr int kTexH = kNy * kNz * kCascades;

void RegisterCVars();
bool Enabled();     // probeGi on

// Steps the field for the camera. False with probeGi off or nothing traced yet. probes: kTexW x kTexH float4 texels,
// serial moves when they changed; grid[c] = (world corner of probe 0,0,0, spacing); params = (strength, normal bias
// in spacings, chebyshev, 0); ambient = (the stock ambient rgb, 0).
bool Update(const float camera[3], const float*& probes, uint32_t& serial, float grid[kCascades][4], float params[4], float ambient[4]);

}  // namespace ProbeGi
