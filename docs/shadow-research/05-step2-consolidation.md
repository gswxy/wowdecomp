# 05 — Step 2: one sun-visibility answer per pixel (shadowUnified), 2026-10-25

## Problem
Two shadow answers met on every outdoor pixel: the receivers' own cascade sample (M2 / WMO: lit x (0.3 S' + 0.7) with the
terminator lift; Terrain3: 0.3 min(S, MCSH) + 0.7, no lift) and the shadow mask (VFogShadowMask.hlsl), which divided the
receivers' term back out (ratio = target / stock) and faded the result with the fog, while the receivers' own term never
faded. Results: a near tree's cascade shadow re-printed on a far mountain the land already shadows (debug 2 black there,
the silhouette still on screen), ground vs model strength mismatch at a low sun, cut-outs where the two disagree.

## Options weighed
1. **Screen-space visibility read by the receivers** (Legion style). Needs the mask before the lit draws: a depth
   pre-pass (every opaque draw twice) or last frame's depth (reprojection, disocclusion holes). Every receiver
   permutation (about 682 stock records x D3D9 bytecode / SM4 / SPIR-V) would need a new sampler read. Largest.
2. **Shared HLSL include in every receiver.** The receivers are stock bytecode, not HLSL; the "include" would be a
   token-stream rewrite of the shadow maths in every permutation on three backends, plus terrain_fade.py. The land
   heightfield would need binding to every receiver. Large, and the M2/WMO maths is spread across each record.
3. **Chosen: receivers drop their cascade term, the mask owns it.** The mask already runs after the opaque world,
   already reads the unit map and bands 0-2 with the receivers' own taps and cutovers, the land, the terminator lift
   and the fog. Exterior receivers (shadow value >= 2) simply select the existing value-0 permutations (no shadow) for
   the draws before the mask, and the mask applies min(cascade, land) once. No new receiver permutations, no
   pre-pass, nothing per backend in C++: the switch is `CShadowCache::ReceiverValue` at the shadow-value sites.

Lighting with a multiply after the fact darkens the whole lit colour, not just the direct sun term; that is what the
stock receivers did too (lit x (1 - k (1 - S))), so the ratio is exact for the cascade term: target / lit = 1 - k (1 - S'),
independent of the receiver's own light. The land term keeps the old estimate (ambient / (A + D N.L)).

## What tells the mask which pixels it owns
Receivers that still shade themselves in unified mode: interior WMO groups and interior-lit M2s (value 1, unit map),
everything at shadow quality 1-2 (value 1). Their pixel shaders declare the shadow sampler (s4; s5 for terrain), so
normals.py (the G-buffer injection already shipped for all three backends) writes G-buffer alpha 0.25 + 0.5 w there
(0.25 / 0.75), and w (0 / 1) everywhere else. a > 0.5 still means "normal present" for SSAO / lights / the mask. The mask
treats 0.125 < a < 0.875 as self-shadowed and keeps the old ratio for it; everything else (including unwritten pixels:
fixed function, models that never had a cascade term) gets the unified answer.

Gates (host, VolumetricFog / VolumetricFogRenderer):
- `CShadowCache::s_receiversUnified` is latched before the terrain (CWorldSceneRender, after the planar reflection,
  which the mask does not cover) from `s_unifiedNext`, and cleared in OnSolidDone right after the mask: the blended
  M2 passes, liquids and anything else after the mask shade themselves as before.
- `s_unifiedNext` = the mask drew this frame with the G-buffer bound, `shadowUnified` on (and shadowMask / gfxTerrainShadow
  / a WDL), and the camera not indoors (ShadowQuery's steep-light hysteresis, CShadowQuery::CameraIndoors). Indoors
  unit models (value 0, which the mask cannot tell apart from switched receivers) would otherwise get the exterior
  shell's band shadow; indoors is stock. One frame lag on every gate change.
- The mask runs with no land texture too (light under the horizon), and indoors while unified receivers drew.

## The unified combine (VFogShadowMask.hlsl, c71.z = 1 and not self-shadowed)
    S'     = lerp(min(unit, band 0/1/2), 1, terminatorLift(N))          (all receivers now, terrain included)
    casc   = 1 - k (1 - S')                                              (k = 0.3 gfxShadowFade, the stock strength)
    landT  = lerp(min(A / lit, 1 - k (1 - lift)), 1, land x facing)       (as before: ambient, or a full cascade shadow)
    landR  = lerp(1, landT, strength)                                     (strength = (1 - interiorBlend) gfxShadowFade)
    ratio  = lerp(1, min(casc, landR), 1 - fog)                           (one fog rule)
A tree's shadow inside a mountain's: casc >= landT whenever land = 0, so the min is the mountain's colour.
shadowDebug 2 shows that ratio (grey) on unified pixels: the value the screen gets. Debug 4-7 still show raw terms.

## Behaviour changes to expect
- Terrain gets the M2/WMO terminator lift (no more ground darker than models at a low sun).
- Terrain no longer takes min(S, MCSH) on the cascade side: Terrain1 applies MCSH the stock q0 way, then the mask the
  cascades. Where MCSH and a cascade shadow overlap the ground darkens twice (as it did for the land term already).
- Unit models (players, creatures) outdoors now receive cascade shadows (tree / building / own unit-map shadow); they
  never did (no 0x10 lighting flag). Watch for self-shadow acne on characters.
- Emissive opaque materials (M2 material flag 1) are darkened by the mask too (the land term already did).

## Shadow debug findings (user screenshots 2026-10-25, debug 2)
- Far mountain: black in debug 2 but the tree silhouette on screen: the receivers' unfaded cascade term (fixed by this).
- Rock M2s flat black / white per triangle: debug 2 showed min(cascade, land x facing), and `facing` flips per face where
  the normal is rebuilt from depth (no G-buffer normal). On back faces N.L = 0, so lit = A and the land target is the
  receiver's own colour: the final image did not change there. Debug 2 now shows the applied multiplier instead.
- Slope at ~0.25 not ~0.1: debug 2 showed the lifted cascade (S' rises toward the terminator), not the darkening.
- Hard straight edge with a stair notch (07-40-10, y 1190-1210): coarse land texels (33 yd) or the fine/coarse blend at
  the fine window's edge; not addressed. Check with shadowDebug 4 and shadowMaskFine 0/1.
- Sawtooth along a near rock edge: grazing-angle cascade aliasing; not addressed (slope bias / normal-offset taps).
- Orange patch keeping its texture in debug 2: a surface drawn after the mask (the fading-model route moves models at
  their draw distance from M2 pass 0 to pass 1; or a blended decal). Unaffected by unified (draws after the mask keep
  their own shadow permutations).

## Remaining
- Character self-shadow tuning if acne shows (option: skip unit models — needs a per-draw flag, e.g. their own
  value-1 permutation with a lit cutoff plane, so they tag the G-buffer as self-shadowed).
- Indoors stays stock (step 1's additive interior sun pass is still the open design).
- MCSH vs cascade double darkening on terrain (the mask cannot see MCSH; a G-buffer bit or Terrain1 dropping MCSH under
  unified would fix it).
- Coarse land edge notch, rock-edge sawtooth (above).
