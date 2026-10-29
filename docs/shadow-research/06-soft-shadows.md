# 06 — Contact-hardening model shadows, throw clamp, shadowSunInfo (2026-10-25)

All in VFogShadowMask.hlsl (SoftCompare), on shadowUnified pixels only: the self-shadowed receivers (G-buffer a 0.25 /
0.75) and shadowUnified 0 keep the receivers' own 5-tap compare, so the old ratio stays exact for them.

## Blocker search from compares
hwPCF 1 (default) binds the cascades and the unit map as depth textures read through the hardware compare: there is no
raw depth to average. A compare at a reference moved L yd toward the light reads lit iff the caster is within L yd of
the receiver along the light, so 5 search taps (centre + 4 diagonals at the widest penumbra's radius) at L = 0 and
four offsets L1..L4 (c81) give the share of casters further back than each L; the mean caster distance d is the sum of
the slices at their middles. The same code runs on R32F maps (point compare). Each map's q.z and uv per yard are the
lengths of its ortho rows (no new constants per map).

## Filter
radius = d x 0.00465 x shadowPenumbra (the sun disc 0.0093 rad across; default 3). The receivers' 5 taps scaled by
radius / their own offset (never under 1: the stock pattern at contact), plus 4 rotated taps at half the radius once
wider, so a wide penumbra ramps instead of showing five copies. Lit pixels cost the 5 L = 0 taps per map read.

## shadowMaxThrow (default 0 = off), shadowThrowFade (20)
L3 = max - fade, L4 = max (L1, L2 = L3 / 4, / 2): casters past L4 are dropped from the search's shadowed share, half of
those between L3 and L4; the filtered shadow is scaled by that share. It applies to every cascade caster, ADT terrain
included (shadowTerrainCast): the land heightfield term (shadowMask, fine 4.2 yd level within 1 km) is not clamped and
carries the mountains at full length, so a separate terrain depth map was not added. With shadowMask or
gfxTerrainShadow off a clamped terrain shadow is cut at the limit too. Off by default: a tree on a ridge crest does
shadow a valley at a low sun (user, 2026-10-25).

## Leaf transmission
Skipped: the maps hold depth only, nothing tells a leaf caster from a trunk or a wall.

## shadowDebug 8
The caster distance: black at contact to white at L4 (40 yd unclamped), red where shadowMaxThrow fades the shadow, dark
blue where no caster.

## shadowSunInfo (console)
Prints game time, light body, and elevation / bearing of the sun and moon discs, the lighting (m_light1), the shadow
maps (s_sunDir), the gfxSunShadows latch and the land term's light. Code path: SetPlanets' g_sunToward -> LightBody ->
SunShadowDir (elevation clamped 4-70 deg, bearing kept) -> latch -> CShadowCache::PreUpdate (bands and unit map) and
TerrainSunShadow::Update (land): one direction for all shadow terms, the visible disc's. The lighting keeps the disc's
bearing but its height is capped at the stock light's for the hour (gfxSunArcMatch 1), so shading can be lower than
the shadows' sun, by design (the DBC colours). Under gfxSunArc the sun's zenith angle is linear in time between 05:30
(100 deg) and 11:55 (28 deg): at 09:00 the sun is 29.3 deg up, bearing ~138 (south-east): shadows 1.8x a caster's
height on flat ground, and much longer down a slope falling away from the sun.
