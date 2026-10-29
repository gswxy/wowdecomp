// FarTreesVS (vs_3_0): far trees as wxl-pack billboards (WoW/Source/WorldClient/FarTrees.cpp,
// fartrees/FARTREES-PLAN.md section 9), a port of wxl-farfield FarFoliage.cpp kVsTree. One card per placement from
// a static per-tile buffer (6 vertices, GxVBF_PT2): a square of side frame x scale, its bottom edge on the
// placement's z + bottom x scale, standing along the placement's lean axis and turned about it to face the camera.
// Its picture is the nearer of the model's 4 side views (every 90 deg about the model's z, FarTreesBake), crossing
// over to the next only within view_blend deg of the 45 deg line.
//   POSITION  = the placement's origin (world)
//   TEXCOORD0 = x: corner (0..3) + yaw / 2pi (rot.y + 180 deg), y: size class x 64 + scale (0: not drawn)
//   TEXCOORD1 = the model's up axis in the world, x and y (FarTrees.cpp Lean)
float4 gM[4]   : register(c0);   // rows of camera-relative world -> clip (row vectors, native projection)
float4 gCam    : register(c4);   // xyz camera, w: the far plane for the hand-over (min(farclip, 2133 - a chunk's diagonal))
float4 gModel  : register(c5);   // x: frame, y: bottom (model space)
float4 gMaxD   : register(c6);   // xyz: the game's cull distance D of classes 2, 3, 4 (fade start + stretch), w: tree_lead
float4 gBand   : register(c7);   // xyz: its fade stretch B of classes 2, 3, 4, w: tree_band
float4 gView   : register(c8);   // x: px per (yd / view depth), y: min px, z: fogStart, w: 1 / (fogEnd - fogStart)
float4 gSun    : register(c9);   // xyz toward the sun (world)
// farTreesBeyond (FarTrees.cpp DrawTrees): the near pass draws the trees whose view depth is below the split, the pass
// inside RenderHorizon (the horizon's projection and depth band) the rest, fogged with FarLand's haze.
float4 gPass   : register(c10);  // x: 1 / farLandHazeHeight (0: none), y: most fog (FarLand::kFogMax; 1 near), z: split (view depth), w: +1 near / -1 beyond
float4 gThin   : register(c11);  // x: thinning starts (view depth), y: reach (yd), z: 1 / shrink band, w: 1 = fog by distance (FarLand), 0 = by view depth

struct VsOut {
    float4 pos   : POSITION;
    float4 uv    : TEXCOORD0;   // the two views' uv
    float4 info  : TEXCOORD1;   // x: how far on toward the second view (0..1), y: hand-over (0..1), z: fog
    float4 right : TEXCOORD2;   // xyz: the card's right (world)
    float4 up    : TEXCOORD3;   // xyz: its up (the lean axis)
    float4 toCam : TEXCOORD4;   // xyz: toward the camera (unit)
    float4 rel   : TEXCOORD5;   // xyz: the card point camera-relative (world), nudged toward the sun (shadowFarReceivers)
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

    // Beyond farclip a 1-2 px card shimmers: past gThin.x keep a share p ~ 1 / depth of the trees (a hash of the
    // placement), never grown; the last stretch to the reach shrinks them to nothing (no line at the placed tiles' edge).
    float  wb   = dot(base - gCam.xyz, float3(gM[0].w, gM[1].w, gM[2].w)) + gM[3].w;
    float  p    = clamp(gThin.x / max(wb, 1.0), 0.25, 1.0);
    float  hash = frac(dot(frac(base.xy * float2(0.1031, 0.1173)), float2(41.37, 27.91)));
    side *= saturate((gThin.y - length(base - gCam.xyz)) * gThin.z);

    float3 toCam = gCam.xyz - base;
    float2 flat  = normalize(toCam.xy + float2(1e-4, 0.0));
    float3 up    = float3(leanIn, sqrt(saturate(1.0 - dot(leanIn, leanIn))));
    float3 rl    = cross(up, toCam);
    float3 right = dot(rl, rl) > 1e-6 ? normalize(rl) : float3(-flat.y, flat.x, 0.0);
    float3 pw    = base + right * ((q.x - 0.5) * side) + up * (q.y * side);

    float dist = length(toCam);
    // The game's own hand-over (World__QueueDoodadDraw: alpha = 1 - (d - (D - B)) / B), one stretch early: the card
    // comes in under its still-solid tree and is whole where the tree begins to thin.
    float  maxD    = cls > 3.5 ? gMaxD.z : (cls > 2.5 ? gMaxD.y : gMaxD.x);
    float  band    = max(cls > 3.5 ? gBand.z : (cls > 2.5 ? gBand.y : gBand.x), 1.0);
    float  byClass = saturate((dist - (maxD - 2.0 * band)) / band);
    float  byPlane = saturate((dist - (gCam.w - gMaxD.w - gBand.w)) / gBand.w);
    float  vis     = max(byClass, byPlane);
    // While the game's tree may still be there (short of its cull distance, a chunk's diagonal over), the card stands
    // half its width further off along the view ray: the same on screen, but behind its tree, which hides it as it
    // comes in; eased out past that, so it is back on its ground once it stands alone.
    const float kChunk = 47.14;
    float back = 0.5 * side * saturate((maxD + kChunk + 20.0 - dist) / 20.0);
    pw -= toCam / max(dist, 1e-3) * back;

    float3 cw = base + up * (0.5 * side);   // the card's middle: size on screen, fog
    float  wc = dot(cw - gCam.xyz, float3(gM[0].w, gM[1].w, gM[2].w)) + gM[3].w;
    float  px = side * gView.x / max(wc, 1.0);
    // fog: stock linear by view depth (near), or FarLand's haze (beyond): the ray's length times the mean density
    // e^-(rise / farLandHazeHeight) along it, capped at kFogMax so the far canopy stays a silhouette with the land
    float  x   = max((cw.z - gCam.z) * gPass.x, 1e-4);
    float  fd  = lerp(wc, length(cw - gCam.xyz) * (1.0 - exp(-x)) / x, gThin.w);
    float  fog = min(saturate((fd - gView.z) * gView.w), gPass.y);
    bool   off = t.y == 0.0 || vis <= 0.002 || px < gView.y || hash > p || (wb - gPass.z) * gPass.w >= 0.0 ||
                 saturate((wc - 0.5 * side - gView.z) * gView.w) * step(gThin.w, 0.5) >= 0.999;
    float4 cc  = (pw.x - gCam.x) * gM[0] + (pw.y - gCam.y) * gM[1] + (pw.z - gCam.z) * gM[2] + gM[3];
    o.pos = off ? float4(0.0, 0.0, -2.0, 1.0) : cc;   // outside the clip volume: the card is dropped

    // Which of the four views, and how far on toward the next: the camera's direction round the tree less the tree's
    // own turn (rot.y + 180 deg, in yaw already).
    float f  = (atan2(flat.y, flat.x) - yaw) / 1.5707963;
    f -= floor(f / 4.0) * 4.0;
    float v0 = floor(f), v1 = v0 + 1.0 - (v0 > 2.5 ? 4.0 : 0.0);
    // Kept half a level-1 texel inside the view's own slot, so filtering never reaches the next view.
    float2 cell = clamp(float2(q.x, 1.0 - q.y), 1.0 / 64.0, 63.0 / 64.0);
    o.uv    = float4((v0 + cell.x) * 0.25, cell.y, (v1 + cell.x) * 0.25, cell.y);
    o.info  = float4(f - v0, vis, fog, 0.0);
    o.right = float4(right, 0.0);
    o.up    = float4(up, 0.0);
    o.toCam = float4(toCam / max(dist, 1e-3), 0.0);
    // ponytail: a fixed 0.08 side toward the sun keeps the card off its own farTreesShadow caster; a per-band texel
    // bias if acne still shows.
    o.rel = float4(pw - gCam.xyz + gSun.xyz * (0.08 * side), 0.0);
    return o;
}
