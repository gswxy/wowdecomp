"""Per-texel metal classifier for a mail/plate item texture (the reference for ArmorMask.cpp).
in: float32 HxWx4 RGBA in 0..1 (sRGB values, as stored). out: metal 0..1, rough 0..1 (HxW).
Covered texels are metal by default; subtract what looks like leather, cloth, fur or skin by local structure."""
import numpy as np

def ss(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def box(a, r):
    """mean over the (2r+1)^2 window clipped to the image (summed-area table, as the C++)."""
    h, w = a.shape
    s = np.zeros((h + 1, w + 1), np.float64)
    s[1:, 1:] = a.astype(np.float64).cumsum(0).cumsum(1)
    y0 = np.clip(np.arange(h) - r, 0, h); y1 = np.clip(np.arange(h) + r + 1, 0, h)
    x0 = np.clip(np.arange(w) - r, 0, w); x1 = np.clip(np.arange(w) + r + 1, 0, w)
    tot = s[y1][:, x1] - s[y0][:, x1] - s[y1][:, x0] + s[y0][:, x0]
    n = (y1 - y0)[:, None] * (x1 - x0)[None, :]
    return (tot / n).astype(np.float32)

def contrast(y, r):
    m = box(y, r)
    return np.sqrt(np.maximum(box(y * y, r) - m * m, 0.0)), m

def classify(img):
    img = img.astype(np.float32)
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    y = (0.299 * r + 0.587 * g + 0.114 * b).astype(np.float32)
    v = np.maximum(r, np.maximum(g, b))
    sd1, m1 = contrast(y, 1)
    sd2, m2 = contrast(y, 6)
    con = np.maximum(sd1 * 2.5, sd2 * 1.5)          # the value range near and around the texel
    flat = 1.0 - ss(0.06, 0.16, con)                # soft, low contrast: cloth panels, leather, fur
    dark = 1.0 - ss(0.14, 0.30, v)                  # dark and matte: leather, straps
    leather = dark * (1.0 - ss(0.08, 0.20, con))
    # brown leather: warm (r >= g >= b), mid-saturated, not bright, no highlight above its surroundings
    mx = v; mn = np.minimum(r, np.minimum(g, b))
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    warm = ((r >= g) & (g >= b)).astype(np.float32)
    hi = np.maximum(y - m2, 0.0)
    brown = warm * ss(0.2, 0.35, sat) * (1.0 - ss(0.38, 0.55, v)) * (1.0 - ss(0.06, 0.14, hi))
    nonmetal = np.maximum(np.maximum(leather, flat), brown)
    nonmetal = nonmetal * (1.0 - ss(0.5, 0.65, y))  # bright (silver, gold, polished paint) stays metal
    metal = (1.0 - nonmetal) * ss(0.04, 0.10, v)    # black gaps are no metal either
    rough = np.clip(0.6 - 1.2 * np.maximum(y - m2, 0.0), 0.25, 0.7)  # brighter than around: polished
    return metal.astype(np.float32), rough.astype(np.float32)
