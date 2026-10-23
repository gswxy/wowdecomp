// FarTreesShadowVS (vs_3_0): far tree impostors as sun shadow casters (WoW/Source/WorldClient/FarTrees.cpp
// DrawShadow, from CShadowQuery::RenderCallback into cascades 0-2). Same static per-tile buffer as FarTreesVS
// (GxVBF_PT, 6 vertices a tree). One quad per tree on the atlas view nearest the light (hemi-octahedral grid,
// FarTreesVS), so its picture is the tree's silhouette as the sun sees it; FarTreesShadowPS cuts it out and writes
// the stock ShadowMap VS/PS's light view depth.
//   POSITION  = the placement's origin (world)
//   TEXCOORD0 = x: corner (0..3) + yaw / 2pi, y: scale, negative for stock size class 3, 0: not drawn (FarTrees.cpp SizeClass)
float4 gM[4]   : register(c0);   // rows of camera-relative world -> band clip (row vectors, native projection)
float4 gCam    : register(c4);   // xyz: the camera the band's view is relative to
float4 gModel  : register(c5);   // xyz: impostor centre (model space), w: radius R (model space)
float4 gGrid   : register(c6);   // x: F, y: F - 1, z: (unused), w: 1 / F
float4 gFade   : register(c7);   // x, y: fade start of levels 2 and 3 (yd)
float4 gEye    : register(c8);   // xyz: CWorldScene::s_activeWorldView, the centre of the doodad fade
float4 gSun    : register(c9);   // xyz toward the light (world)
float4 gViewZ  : register(c10);  // camera-relative world -> light view z (the column the stock VS's c33 holds)

struct VsOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 z   : TEXCOORD1;      // z: light view depth, as the stock ShadowMap VS's TEXCOORD1
};

float3 Decode(float2 g)   // grid point (0..F-1) -> view direction (model space, z up); FarTreesVS
{
    float2 p = g / gGrid.y * 2.0 - 1.0;
    float2 h = float2(p.x + p.y, p.x - p.y) * 0.5;
    return normalize(float3(h, 1.0 - abs(h.x) - abs(h.y)));
}

VsOut main(float3 base : POSITION, float2 t : TEXCOORD0)
{
    VsOut o;
    float corner = floor(t.x);
    float yaw    = frac(t.x) * 6.2831853;
    float scale  = abs(t.y);
    float cs = cos(yaw), sn = sin(yaw);

    float3 c  = gModel.xyz * scale;
    float3 cw = base + float3(c.x * cs - c.y * sn, c.x * sn + c.y * cs, c.z);
    float  R  = gModel.w * scale;

    // Hand-over: the real doodad casts while it is nearer than its fade start (CWorldScene::IsBeyondFadeDistance,
    // CShadowQuery's queries), the impostor from there on: one of them, never both, no gap.
    float start = t.y < 0.0 ? gFade.y : gFade.x;
    bool  off = scale == 0.0 || length(cw - gEye.xyz) <= start;

    // The grid view nearest the light, in the model's frame (the sun is above: the hemisphere holds it).
    float3 lm = float3(gSun.x * cs + gSun.y * sn, -gSun.x * sn + gSun.y * cs, max(gSun.z, 0.0));
    float  s  = abs(lm.x) + abs(lm.y) + lm.z + 1e-4;
    float2 g  = floor(saturate(float2(lm.x + lm.y, lm.x - lm.y) / s * 0.5 + 0.5) * gGrid.y + 0.5);
    float3 dm = Decode(g);
    float3 rm = normalize(cross(float3(0.0, 0.0, 1.0), dm) + float3(1e-5, 0.0, 0.0));
    float3 um = cross(dm, rm);
    float3 rw = float3(rm.x * cs - rm.y * sn, rm.x * sn + rm.y * cs, rm.z);
    float3 uw = float3(um.x * cs - um.y * sn, um.x * sn + um.y * cs, um.z);

    // The quad that view was baked on (side 2R through the centre, across dm): its corners are the cell's corners.
    float2 k = float2(corner == 1.0 || corner == 2.0 ? 1.0 : -1.0, corner >= 2.0 ? 1.0 : -1.0);
    float3 p = cw + (rw * k.x + uw * k.y) * R - gCam.xyz;
    float4 cc = p.x * gM[0] + p.y * gM[1] + p.z * gM[2] + gM[3];
    o.pos = off ? float4(0.0, 0.0, -2.0, 1.0) : cc;   // outside the clip volume: the quad is dropped
    o.uv  = (g + float2(k.x, -k.y) * 0.5 + 0.5) * gGrid.w;
    o.z   = float4(0.0, 0.0, dot(p, gViewZ.xyz) + gViewZ.w, 0.0);
    return o;
}
