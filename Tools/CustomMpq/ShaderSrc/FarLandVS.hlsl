// farLand (WorldClient/FarLand.cpp): the stock WDL horizon mesh (GxVBF_PC, world positions, CMap::RenderLowDetail's
// own buffers) laid with the map's baked atlases (Tools/FarLand/bake_farland.py). The tile's slot comes in c5.
float4 gM[4] : register(c0);   // camera-relative world -> clip (row vectors, native projection: the horizon's)
float4 gCam  : register(c4);   // xyz camera
float4 gTile : register(c5);   // tile top-left world x, y; slot origin u, v (0..1)
float4 gUvA  : register(c6);   // albedo atlas: (S-1)/W, (S-1)/H, 0.5/W, 0.5/H
float4 gUvN  : register(c7);   // normal atlas: the same for its own S, W, H
struct In  { float4 p : POSITION; float4 c : COLOR0; };
struct Out { float4 pos : POSITION; float4 uv : TEXCOORD0; float dist : TEXCOORD1; };
Out main(In i)
{
    Out o;
    float3 rel = i.p.xyz - gCam.xyz;
    o.pos = rel.x * gM[0] + rel.y * gM[1] + rel.z * gM[2] + gM[3];
    // u along the tile's columns (world -Y), v along its rows (world -X), as the bake; edge -> edge texel centre
    float2 t = saturate(float2(gTile.y - i.p.y, gTile.x - i.p.x) * (3.0 / 1600.0));
    o.uv.xy = gTile.zw + gUvA.zw + t * gUvA.xy;
    o.uv.zw = gTile.zw + gUvN.zw + t * gUvN.xy;
    o.dist = length(rel);
    return o;
}
