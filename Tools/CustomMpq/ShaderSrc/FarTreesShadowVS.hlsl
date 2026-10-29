// FarTreesShadowVS (vs_3_0): far tree cards as sun shadow casters (WoW/Source/WorldClient/FarTrees.cpp DrawShadow,
// from CShadowQuery::RenderCallback into cascades 0-2). Same static per-tile buffer as FarTreesVS (GxVBF_PT2,
// 6 vertices a tree): the card stands along the lean axis as there, but turned to face the sun, on the view nearest
// the sun's direction round the tree; FarTreesShadowPS cuts it out and writes the stock ShadowMap VS/PS's light depth.
//   POSITION  = the placement's origin (world)
//   TEXCOORD0 = x: corner (0..3) + yaw / 2pi, y: size class x 64 + scale (0: not drawn)
//   TEXCOORD1 = the model's up axis in the world, x and y
float4 gM[4]   : register(c0);   // rows of camera-relative world -> band clip (row vectors, native projection)
float4 gCam    : register(c4);   // xyz: the camera the band's view is relative to
float4 gModel  : register(c5);   // x: frame, y: bottom (model space)
float4 gFade   : register(c7);   // xyz: fade start of classes 2, 3, 4 (yd)
float4 gEye    : register(c8);   // xyz: CWorldScene::s_activeWorldView, the centre of the doodad fade
float4 gSun    : register(c9);   // xyz toward the light (world)
float4 gViewZ  : register(c10);  // camera-relative world -> light view z (the column the stock VS's c33 holds)

struct VsOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 z   : TEXCOORD1;      // z: light view depth, as the stock ShadowMap VS's TEXCOORD1
};

VsOut main(float3 base : POSITION, float2 t : TEXCOORD0, float2 leanIn : TEXCOORD1)
{
    VsOut o;
    float corner = floor(t.x);
    float yaw    = frac(t.x) * 6.2831853;
    float cls    = floor(t.y / 64.0);
    float scale  = t.y - cls * 64.0;
    float2 q     = float2(corner == 1.0 || corner == 2.0 ? 1.0 : 0.0, corner >= 2.0 ? 1.0 : 0.0);
    float side   = gModel.x * scale;
    base.z += gModel.y * scale;

    // Hand-over: the real doodad casts while it is nearer than its fade start (CWorldScene::IsBeyondFadeDistance,
    // CShadowQuery's queries), the card from there on: one of them, never both, no gap.
    float start = cls > 3.5 ? gFade.z : (cls > 2.5 ? gFade.y : gFade.x);
    bool  off = t.y == 0.0 || length(base - gEye.xyz) <= start;

    float2 flat  = normalize(gSun.xy + float2(1e-4, 0.0));
    float3 up    = float3(leanIn, sqrt(saturate(1.0 - dot(leanIn, leanIn))));
    float3 rl    = cross(up, gSun.xyz);
    float3 right = dot(rl, rl) > 1e-6 ? normalize(rl) : float3(-flat.y, flat.x, 0.0);
    float3 p = base + right * ((q.x - 0.5) * side) + up * (q.y * side) - gCam.xyz;
    float4 cc = p.x * gM[0] + p.y * gM[1] + p.z * gM[2] + gM[3];
    o.pos = off ? float4(0.0, 0.0, -2.0, 1.0) : cc;   // outside the clip volume: the card is dropped

    float f = (atan2(flat.y, flat.x) - yaw) / 1.5707963 + 0.5;   // the nearest view
    f -= floor(f / 4.0) * 4.0;
    float2 cell = clamp(float2(q.x, 1.0 - q.y), 1.0 / 64.0, 63.0 / 64.0);
    o.uv = float2((floor(f) + cell.x) * 0.25, cell.y);
    o.z  = float4(0.0, 0.0, dot(p, gViewZ.xyz) + gViewZ.w, 0.0);
    return o;
}
