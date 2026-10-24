// FarTreesVS (vs_3_0): far trees as hemi-octahedral impostors (WoW/Source/WorldClient/FarTrees.cpp,
// fartrees/FARTREES-PLAN.md). One camera-facing quad per placement from a static per-tile buffer (6 vertices,
// GxVBF_PT). The model's atlas holds F x F orthographic views on the upper hemisphere (hemi-octahedral grid,
// Tools/FarTrees/bake.py). Here: pick the 4 grid views around the camera's direction in the model's frame and
// give the pixel shader each view's uv of this vertex (ray through the vertex hits that view's plane), so the
// picture belongs to the model's turn, never to the camera's.
//   POSITION  = the placement's origin (world)
//   TEXCOORD0 = x: corner (0..3) + yaw / 2pi, y: scale, negative for stock size class 3, 0: not drawn (FarTrees.cpp SizeClass)
float4 gM[4]   : register(c0);   // rows of camera-relative world -> clip (row vectors, native projection)
float4 gCam    : register(c4);   // xyz camera
float4 gModel  : register(c5);   // xyz: impostor centre (model space), w: radius R (model space)
float4 gGrid   : register(c6);   // x: F, y: F - 1, z: (unused), w: 1 / F
float4 gFade   : register(c7);   // fade start of levels 2 and 3 (yd), fade range of levels 2 and 3
float4 gView   : register(c8);   // x: px per (yd / view depth), y: min px, z: fogStart, w: 1 / (fogEnd - fogStart)
float4 gSun    : register(c9);   // xyz toward the sun (world), w: depth bias (fraction of R) during the hand-over

struct VsOut {
    float4 pos  : POSITION;
    float4 uv01 : TEXCOORD0;   // view 0 uv, view 1 uv
    float4 uv23 : TEXCOORD1;
    float4 w    : TEXCOORD2;   // bilinear weights of the 4 views
    float4 clip : TEXCOORD3;   // this vertex's clip z, w; their change per unit of baked depth (toward the camera)
    float4 rot  : TEXCOORD4;   // xy: cos, sin of the yaw; z: fog factor; w: depth bias
    float4 sun  : TEXCOORD5;   // xyz toward the sun in model space
    float4 rel  : TEXCOORD6;   // xyz: the quad point camera-relative (world), nudged toward the sun (shadowFarReceivers)
    float4 relD : TEXCOORD7;   // xyz: its move per unit of baked depth (dw R)
};

float3 Decode(float2 g)   // grid point (0..F-1) -> view direction (model space, z up)
{
    float2 p = g / gGrid.y * 2.0 - 1.0;
    float2 h = float2(p.x + p.y, p.x - p.y) * 0.5;
    return normalize(float3(h, 1.0 - abs(h.x) - abs(h.y)));
}

float2 ViewUv(float3 d, float3 q)   // q = point relative to the centre (model space, scaled to R = 1)
{
    float3 r = normalize(cross(float3(0.0, 0.0, 1.0), d) + float3(1e-5, 0.0, 0.0));
    float3 u = cross(d, r);
    float3 hit = q - d * dot(q, d);   // orthographic view: drop the depth along d
    return float2(dot(hit, r), -dot(hit, u)) * 0.5 + 0.5;
}

VsOut main(float3 base : POSITION, float2 t : TEXCOORD0)
{
    VsOut o;
    float corner = floor(t.x);
    float yaw    = frac(t.x) * 6.2831853;
    float scale  = abs(t.y);
    float cs = cos(yaw), sn = sin(yaw);

    // Model space -> world: rotate about z by yaw, scale, add the origin.
    float3 c  = gModel.xyz * scale;
    float3 cw = base + float3(c.x * cs - c.y * sn, c.x * sn + c.y * cs, c.z);
    float  R  = gModel.w * scale;

    float3 toCam = gCam.xyz - cw;
    float  dist  = length(toCam);
    float3 dw    = toCam / max(dist, 1e-3);
    float3 dm    = float3(dw.x * cs + dw.y * sn, -dw.x * sn + dw.y * cs, dw.z);   // world -> model
    dm.z = max(dm.z, 0.0);
    dm = normalize(dm + float3(0.0, 0.0, 1e-4));

    // Camera-facing quad of side 2R through the centre.
    float3 rw = normalize(cross(float3(0.0, 0.0, 1.0), dw) + float3(1e-5, 0.0, 0.0));
    float3 uw = cross(dw, rw);
    float2 k  = float2(corner == 1.0 || corner == 2.0 ? 1.0 : -1.0, corner >= 2.0 ? 1.0 : -1.0);
    float3 pw = cw + (rw * k.x + uw * k.y) * R;

    // Hand-over: the real doodad fades over [start, start + range] (CWorldScene::s_fadeDistMin / s_fadeRangeDefault
    // of its size class, the class as stock gives it, from the CPU); the impostor is there from start - 2 yd on,
    // opaque, behind it by the bias until the end.
    bool  lvl3  = t.y < 0.0;
    float start = (lvl3 ? gFade.y : gFade.x) - 2.0;
    float range = lvl3 ? gFade.w : gFade.z;
    float4 cc = (pw.x - gCam.x) * gM[0] + (pw.y - gCam.y) * gM[1] + (pw.z - gCam.z) * gM[2] + gM[3];
    // Size on screen and fog from the centre (all 6 vertices alike: a quad is never split); fog at the front (w - R).
    float  wc   = dot(cw - gCam.xyz, float3(gM[0].w, gM[1].w, gM[2].w)) + gM[3].w;
    float px  = 2.0 * R * gView.x / max(wc, 1.0);
    bool  off = scale == 0.0 || dist < start || px < gView.y || saturate((wc - R - gView.z) * gView.w) >= 0.999;
    o.pos = off ? float4(0.0, 0.0, -2.0, 1.0) : cc;   // outside the clip volume: the quad is dropped

    // Each of the 4 views around dm: uv of this corner (model space, R = 1).
    float  s = abs(dm.x) + abs(dm.y) + dm.z;
    float2 g = saturate(   // hemi-oct encode
        (float2(dm.x + dm.y, dm.x - dm.y) / s) * 0.5 + 0.5) * gGrid.y;
    float2 g0 = min(floor(g), gGrid.y - 1.0);
    float2 f  = g - g0;
    float3 pm = pw - cw;
    pm = float3(pm.x * cs + pm.y * sn, -pm.x * sn + pm.y * cs, pm.z) / R;
    float2 cell = float2(1.0, 1.0) * gGrid.w;
    o.uv01.xy = (g0 + ViewUv(Decode(g0), pm)) * cell;
    o.uv01.zw = (g0 + float2(1, 0) + ViewUv(Decode(g0 + float2(1, 0)), pm)) * cell;
    o.uv23.xy = (g0 + float2(0, 1) + ViewUv(Decode(g0 + float2(0, 1)), pm)) * cell;
    o.uv23.zw = (g0 + float2(1, 1) + ViewUv(Decode(g0 + float2(1, 1)), pm)) * cell;
    o.w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);

    // Baked depth d in [-1, 1] (toward the camera, x R): the surface sits d R from the quad toward the camera.
    float4 dc = (dw.x * gM[0] + dw.y * gM[1] + dw.z * gM[2]) * R;
    o.clip = float4(cc.z, cc.w, dc.z, dc.w);
    float fog = saturate((cc.w - gView.z) * gView.w);
    float bias = dist < start + 2.0 + range ? gSun.w : 0.0;
    o.rot = float4(cs, sn, fog, bias);
    o.sun = float4(gSun.x * cs + gSun.y * sn, -gSun.x * sn + gSun.y * cs, gSun.z, 0.0);
    // ponytail: a fixed 0.15 R toward the sun keeps the impostor off its own farTreesShadow caster; a per-band
    // texel bias if acne still shows.
    o.rel = float4(pw - gCam.xyz + gSun.xyz * (0.15 * R), 0.0);
    o.relD = float4(dw * R, 0.0);
    return o;
}
