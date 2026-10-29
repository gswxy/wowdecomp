# 07 — Land shadow edge: crest continuity and a B-spline read (2026-10-25)

## Symptom
shadowDebug 2 (user screenshots 09-06 .. 09-07, Valley of Trials): hill and spire shadows with hard, straight,
stepped shard edges at the near terrain's 4 yd grid (05's "hard edge with a stair notch" on the land term).

## Cause
The land term is `saturate(1 - (lit - z - 4) / 16)` over a bilinear read of the lit-height field (TerrainSunShadow
Build: lit(p) = max(H(q), lit(q)) - drop, q one cell toward the light). lit(p) leaves out the land at p itself, so at a
crest vertex lit is the far upstream land and one vertex behind it the crest's own height: a jump of the ridge's whole
height inside one cell. Bilinear filtering spreads that jump over the cell, so the margin crosses the 16 yd ramp in a
fraction of a cell at a place set by the grid, not the ridge: hard, stepped edges. Lateral shadow edges (the sweep's
1-texel steps across the light) show the grid the same way.

## Fix
- shadowMaskCrest (yd, default 4, 0 = old): the fine level's texels hold max(lit, H - crest). Continuous at the crest,
  so the edge sits on the ridge between vertices; ground still reads 4 + crest yd under its threshold (the mask's 4 yd
  surface bias), so the ADT triangles' departure from the bilinear vertex heights does not shadow it. The coarse (WDL)
  level is unchanged: real ground sits up to tens of yards off the WDL, and the fog, water and caustics read it.
  Visibility (CPU) keeps the plain lit heights. Changing the CVar rebuilds the fine level.
- shadowMaskSmooth (default 1): the mask reads the fine level (and its fading previous build) as a cubic B-spline from
  4 bilinear taps (c80.w = 512 texels per side, 0 = bilinear), so edges curve through the cells. The B-spline
  approximates: it lowers lit at sharp convex kinks and raises it in concave ones by at most about a sixth of the
  height step across a cell, well inside the 8 yd ground tolerance above.

## Not done
- Physical penumbra from the occluder distance (needs a distance channel in the texture).
- The binary back-face rule (land x facing): per-face normals on flat-shaded rocks still cut there.
- The coarse level's 33 yd steps past 1 km and the fine/coarse seam.
