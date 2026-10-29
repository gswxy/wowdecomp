import sys, io, os, json
import numpy as np
from PIL import Image
import mpq, classify

OUT = os.environ.get('ARMORMASK_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out'))
os.makedirs(OUT, exist_ok=True)
FOLD = ['ArmUpperTexture','ArmLowerTexture','HandTexture','TorsoUpperTexture','TorsoLowerTexture','LegUpperTexture','LegLowerTexture','FootTexture']
# texture field section -> rect in 512 layout (x, y, w, h)
RECT = [(0,0,256,128),(0,128,256,128),(0,256,256,64),(256,0,256,128),(256,128,256,64),(256,192,256,128),(256,320,256,128),(256,448,256,64)]
idi, S = mpq.dbc('ItemDisplayInfo.dbc')
items, _ = mpq.dbc('Item.dbc')
sub = {}
for r in items.values():
    if r[1] == 4 and r[2] <= 4 and r[5]:
        sub.setdefault(r[5], [0]*5)[r[2]] += 1
def kind(disp):  # 0 none/cloth, 1 mail, 2 plate (the C++ PbrArmorCategory)
    c = sub.get(disp)
    if not c:  # NPC-only display (CreatureDisplayInfoExtra): the texture name's material prefix
        r = idi.get(disp)
        n = next((S(x) for x in (r[15:23] if r else ()) if x), '').lower()
        return 2 if n.startswith('plate') else (1 if n.startswith('mail') else 0)
    best = 0
    for k in range(1, 5):
        best = k if c[k] >= c[best] else best
    return 2 if best == 4 else (1 if best == 3 else 0)

def blp(path):
    b = mpq.read(path)
    if b is None: return None
    return decode(b)

def decode(b):
    import struct
    enc, ad = b[8], b[9]
    w, h = struct.unpack_from('<2I', b, 12)
    off = struct.unpack_from('<16I', b, 20)[0]
    if enc == 1:  # palettized: indices then alpha (1/4/8 bits)
        pal = np.frombuffer(b, np.uint8, 1024, 148).reshape(256, 4)[:, [2, 1, 0]]
        n = w * h
        idx = np.frombuffer(b, np.uint8, n, off)
        rgb = pal[idx].astype(np.float32) / 255.0
        if ad == 8:
            a = np.frombuffer(b, np.uint8, n, off + n).astype(np.float32) / 255.0
        elif ad == 4:
            ab = np.frombuffer(b, np.uint8, (n + 1) // 2, off + n)
            a = np.stack([ab & 15, ab >> 4], -1).reshape(-1)[:n].astype(np.float32) / 15.0
        elif ad == 1:
            ab = np.frombuffer(b, np.uint8, (n + 7) // 8, off + n)
            a = np.unpackbits(ab, bitorder='little')[:n].astype(np.float32)
        else:
            a = np.ones(n, np.float32)
        return np.concatenate([rgb, a[:, None]], 1).reshape(h, w, 4)
    a = np.asarray(Image.open(io.BytesIO(b)).convert('RGBA'), dtype=np.float32) / 255.0
    if ad == 0:
        a = a.copy(); a[..., 3] = 1.0
    return a

def comp_tex(name, folder):
    for suf in ('_U', '_M', '_F'):
        t = blp('Item\\TextureComponents\\%s\\%s%s.blp' % (folder, name, suf))
        if t is not None: return t
    return None

def nearest(img, w, h):
    sh, sw = img.shape[:2]
    ys = (np.arange(h) * sh) // h; xs = (np.arange(w) * sw) // w
    return img[ys][:, xs]

def build(displays, skin):
    """displays bottom->top. Returns composite 512 RGBA, mask 256x256x3 (plate, mail, rough)."""
    col = nearest(skin, 512, 512).copy()
    mask = np.zeros((256, 256, 3), np.float32); mask[..., 2] = 1.0
    for d in displays:
        r = idi[d]; k = kind(d)
        for sec in range(8):
            name = S(r[15 + sec])
            if not name: continue
            t = comp_tex(name, FOLD[sec])
            if t is None: print('missing', name, FOLD[sec]); continue
            x, y, w, h = RECT[sec]
            tt = nearest(t, w, h); a = tt[..., 3:4]
            col[y:y+h, x:x+w, :3] = col[y:y+h, x:x+w, :3] * (1 - a) + tt[..., :3] * a
            th = nearest(t, w // 2, h // 2); ah = th[..., 3]
            cov = ah >= 0.5
            sl = mask[y//2:(y+h)//2, x//2:(x+w)//2]
            if k:
                met, rough = classify.classify(th)
                sl[..., 0] = np.where(cov, met * (k == 2), sl[..., 0])
                sl[..., 1] = np.where(cov, met * (k == 1), sl[..., 1])
                sl[..., 2] = np.where(cov, rough, sl[..., 2])
            else:
                sl[cov] = (0, 0, 1)
    return col, mask

def attach(path, k):
    t = blp(path)
    if t is None: print('missing', path); return None
    h, w = t.shape[:2]; mw = min(w, 256); mh = max(1, h * mw // w)
    th = nearest(t, mw, mh)
    met, rough = classify.classify(th)
    # an attachment's alpha is not coverage (helms use it for the stock env/glow masks): ignored
    m = np.zeros((mh, mw, 3), np.float32)
    m[..., 0] = met * (k == 2); m[..., 1] = met * (k == 1); m[..., 2] = rough
    return t, m

def preview(col, mask, path, size=256):
    c = Image.fromarray((np.clip(col, 0, 1)[..., :3] * 255).astype(np.uint8)).resize((size, size * col.shape[0] // col.shape[1]), Image.BILINEAR)
    met = np.maximum(mask[..., 0], mask[..., 1])
    # overlay: metal shown as the texel with a cyan(plate)/orange(mail) tint, non-metal greyed dark
    cm = nearest(col[..., :3], mask.shape[1], mask.shape[0])
    g = cm.mean(-1, keepdims=True) * 0.35
    tint = np.where(mask[..., 0:1] >= mask[..., 1:2], np.array([0.2, 0.9, 1.0]), np.array([1.0, 0.6, 0.1]))
    ov = g * (1 - met[..., None]) + (0.4 * cm + 0.6 * tint) * met[..., None]
    o = Image.fromarray((np.clip(ov, 0, 1) * 255).astype(np.uint8)).resize(c.size, Image.NEAREST)
    im = Image.new('RGB', (c.size[0] * 2 + 4, c.size[1]), (40, 40, 40))
    im.paste(c, (0, 0)); im.paste(o, (c.size[0] + 4, 0)); im.save(path)
