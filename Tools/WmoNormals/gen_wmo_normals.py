"""Generates `_n` normal maps for the stone/brick/wood/plaster textures of the stock 3.3.5a WMOs and packs
them into one MPQ.

    python3 -I Tools/WmoNormals/gen_wmo_normals.py --client /root/tswow-server/tswow-client \
        --out /root/wmonormals-data [--retail-listfile /root/casc/listfile.csv] [-j 8]
    python3 -I Tools/WmoNormals/gen_wmo_normals.py --selftest

Writes <out>/out/<Dir>/<Name>_n.blp (loose), <out>/wmonormals.MPQ, <out>/report.tsv (one line per texture:
path, verdict). Needs StormLib (libstorm.so), numpy, scipy, Pillow.

Steps: every root WMO (not *_NNN.wmo) in the stock archives' listfiles -> MOTX texture paths -> keep
stone/brick/wood/plaster by path keywords, drop alpha-cutout (>5% of pixels alpha < 128) and near-flat
(luma std < 0.02) diffuses -> per texture: Rec.709 luma of mip 0, downsampled to max side 256, high-pass
(minus Gaussian sigma = size/16, wrap), blur sigma 0.7, divided by its std, Sobel (wrap) -> normal ->
BLP2 DXT1 (encoding 2, alphaDepth 0, alphaType 0), full mip chain (box filter + renormalise per level).

Convention (the shader depends on it): u = image column increasing right, v = image row increasing DOWN
(row 0 = first row of mip 0). Height h = high-passed luma (bright = raised);
n = normalize(-dh/du * K, -dh/dv * K, 1); R = nx*0.5+0.5 (along +u), G = ny*0.5+0.5 (along +v),
B = nz*0.5+0.5. Name: Dir\\Name.blp -> Dir\\Name_n.blp, path case as found in MOTX.
"""
import argparse
import ctypes
import io
import os
import re
import struct
import sys
import time
from multiprocessing import Pool

import numpy as np
from PIL import Image
from scipy import ndimage

K = 0.13          # slope per unit of std-normalised Sobel gradient; tuned for a mean tilt of ~15 degrees
MAX_SLOPE = 1.2   # clamp |gradient| (about 50 degrees)
MAX_SIDE = 256

KEEP = re.compile(r'stone|brick|wall|wood|plank|floor|roof|rock|cobble|tile|plaster|beam|trim|pillar|column|'
                  r'stair|block|masonry|timber|board|ceiling|ceeling', re.I)
DROP = re.compile(r'glass|window|light|glow|lamp|fire|flame|water|lava|slime|sky|banner|cloth|fabric|tapestr|'
                  r'rug|carpet|leaf|leaves|foliage|grass|vine|cloud|crystal', re.I)
METAL = re.compile(r'metal|iron|gold', re.I)

# --- archives (StormLib) ------------------------------------------------------------------------------
DWORD, HANDLE = ctypes.c_uint32, ctypes.c_void_p
MPQ_OPEN_READ_ONLY = 0x100
STOCK = ['common.MPQ', 'common-2.MPQ', 'expansion.MPQ', 'lichking.MPQ', 'patch.MPQ', 'patch-2.MPQ', 'patch-3.MPQ']


def storm():
    lib = None
    for name in ('libstorm.so.9', 'libstorm.so'):
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError:
            pass
    if lib is None:
        sys.exit('StormLib not found')
    lib.SFileGetFileSize.restype = DWORD
    return lib


class Archives:
    """The client's archives in load order; read() returns the last one holding a file (later wins)."""

    def __init__(self, data):
        self.lib = storm()
        names = STOCK + sorted(n for n in os.listdir(data) if re.fullmatch(r'patch-[A-Za-z]\.MPQ', n))
        self.order = []          # (name, handle or {lowerpath: fspath})
        for n in names:
            p = os.path.join(data, n)
            if os.path.isfile(p):
                h = HANDLE()
                if not self.lib.SFileOpenArchive(p.encode(), 0, MPQ_OPEN_READ_ONLY, ctypes.byref(h)):
                    sys.exit('cannot open ' + p)
                self.order.append((n, h))
            loose = p[:-4]       # some lettered patches ship as loose folders (patch-A)
            if n not in STOCK and os.path.isdir(loose):
                idx = {}
                for root, _, files in os.walk(loose):
                    for f in files:
                        fp = os.path.join(root, f)
                        idx[os.path.relpath(fp, loose).replace('/', '\\').lower()] = fp
                self.order.append((n[:-4], idx))

    def listfile(self, name):
        data = self._read_from(dict(self.order)[name], '(listfile)')
        return data.decode('latin-1').replace('\r', '').split('\n') if data else []

    def _read_from(self, src, path):
        if isinstance(src, dict):
            fp = src.get(path.lower())
            return open(fp, 'rb').read() if fp else None
        hf = HANDLE()
        if not self.lib.SFileOpenFileEx(src, path.encode('latin-1'), 0, ctypes.byref(hf)):
            return None
        size = self.lib.SFileGetFileSize(hf, None)
        buf, got = ctypes.create_string_buffer(size), DWORD()
        ok = self.lib.SFileReadFile(hf, buf, size, ctypes.byref(got), None) or got.value == size
        self.lib.SFileCloseFile(hf)
        return buf.raw[:got.value] if ok else None

    def read(self, path):
        for _, src in reversed(self.order):
            d = self._read_from(src, path)
            if d is not None:
                return d
        return None


def motx(wmo):
    """Texture paths in a root WMO's MOTX chunk (stored as 'XTOM')."""
    pos = 0
    while pos + 8 <= len(wmo):
        tag, size = wmo[pos:pos + 4], struct.unpack_from('<I', wmo, pos + 4)[0]
        if tag == b'XTOM':
            raw = wmo[pos + 8:pos + 8 + size].split(b'\0')
            return [s.decode('latin-1') for s in raw if s.lower().endswith(b'.blp')]
        pos += 8 + size
    return []


# --- normal map ----------------------------------------------------------------------------------------
def luma(img):
    a = np.asarray(img.convert('RGB'), np.float32) / 255.0
    return a @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def pow2_floor(x):
    return 1 << (max(1, x).bit_length() - 1)


def normal_map(img):
    """PIL image (mip 0 of the diffuse) -> HxWx3 float normals, unit length, in the convention above."""
    w, h = img.size
    s = min(1.0, MAX_SIDE / max(w, h))
    tw, th = pow2_floor(int(w * s)), pow2_floor(int(h * s))
    rgb = img.convert('RGB')
    if (tw, th) != (w, h):
        rgb = rgb.resize((tw, th), Image.BOX)
    L = luma(rgb)
    hp = L - ndimage.gaussian_filter(L, max(th, tw) / 16.0, mode='wrap')
    hp = ndimage.gaussian_filter(hp, 0.7, mode='wrap')
    hp /= hp.std() + 1e-6
    du = ndimage.sobel(hp, axis=1, mode='wrap')      # d/d(column), +u right
    dv = ndimage.sobel(hp, axis=0, mode='wrap')      # d/d(row), +v down
    gx, gy = du * K, dv * K
    mag = np.hypot(gx, gy)
    f = np.minimum(1.0, MAX_SLOPE / (mag + 1e-9))
    n = np.stack([-gx * f, -gy * f, np.ones_like(gx)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


# --- BLP2 DXT1 ---------------------------------------------------------------------------------------------
def _to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def _from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def encode_dxt1(rgb):
    """HxWx3 uint8 (H, W multiples of 4) -> DXT1 blocks, always 4-colour mode (c0 > c1, or all index 0)."""
    h, w, _ = rgb.shape
    c = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.float32)
    n = c.shape[0]
    mean = c.mean(1, keepdims=True)
    d = c - mean
    cov = np.einsum('nki,nkj->nij', d, d)
    axis = np.ones((n, 3), np.float32)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-9
    proj = np.einsum('nki,ni->nk', d, axis)
    c0 = _to565(mean[:, 0] + axis * proj.max(1)[:, None])
    c1 = _to565(mean[:, 0] + axis * proj.min(1)[:, None])
    c0, c1 = np.maximum(c0, c1), np.minimum(c0, c1)
    e0, e1 = _from565(c0), _from565(c1)
    pal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
    idx = ((c[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(3).argmin(2).astype(np.uint32)
    idx[c0 == c1] = 0                                   # equal endpoints = 3-colour mode, index 3 = transparent
    bits = np.zeros(n, np.uint32)
    for i in range(16):
        bits |= idx[:, i] << np.uint32(2 * i)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = c0 & 0xFF, c0 >> 8, c1 & 0xFF, c1 >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> np.uint32(8 * k)) & 0xFF
    return out.tobytes()


def blp_dxt1(normals):
    """Unit normals HxWx3 (power-of-two sides) -> BLP2 bytes, DXT1, mips to 1x1."""
    mips, n = [], normals
    while True:
        h, w, _ = n.shape
        pad = np.zeros((max(4, h), max(4, w), 3), np.uint8)
        pad[:h, :w] = np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255)
        mips.append(encode_dxt1(pad))
        if (h, w) == (1, 1):
            break
        nh, nw = max(1, h // 2), max(1, w // 2)
        n = n.reshape(nh, h // nh, nw, w // nw, 3).mean((1, 3))
        n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
    h, w, _ = normals.shape
    header = struct.pack('<4sIBBBBII', b'BLP2', 1, 2, 0, 0, 1, w, h)
    offs, sizes, pos = [], [], 20 + 128 + 1024
    for m in mips:
        offs.append(pos)
        sizes.append(len(m))
        pos += len(m)
    pad16 = [0] * (16 - len(mips))
    return (header + struct.pack('<16I', *(offs + pad16)) + struct.pack('<16I', *(sizes + pad16)) + b'\0' * 1024
            + b''.join(mips))


def check_blp(data):
    """Pillow decodes it back; mip 0 normals are unit length on average (+-0.1; DXT1 interpolation shortens
    single texels between distant endpoints, so the 95th percentile may sit up to 0.2 off; renormalise in the shader)."""
    img = Image.open(io.BytesIO(data))
    img.load()
    n = np.asarray(img.convert('RGB'), np.float32) / 127.5 - 1.0
    ln = np.linalg.norm(n, axis=-1)
    assert abs(ln.mean() - 1.0) < 0.1 and np.percentile(np.abs(ln - 1.0), 95) < 0.2, (ln.mean(), np.percentile(np.abs(ln - 1.0), [95, 99]))
    return n


# --- per texture (worker) -------------------------------------------------------------------------------
def process(job):
    path, diffuse = job
    try:
        img = Image.open(io.BytesIO(diffuse))
        img.load()
    except Exception as e:
        return path, 'decode-failed: %s' % e, None, 0.0
    if img.mode in ('RGBA', 'LA', 'PA') or 'transparency' in img.info:
        a = np.asarray(img.convert('RGBA'))[..., 3]
        if (a < 128).mean() > 0.05:
            return path, 'alpha-cutout', None, 0.0
    w, h = img.size
    if w & (w - 1) or h & (h - 1):
        return path, 'not-pow2', None, 0.0
    if luma(img).std() < 0.02:
        return path, 'flat', None, 0.0
    n = normal_map(img)
    blp = blp_dxt1(n)
    try:
        check_blp(blp)
    except AssertionError as e:
        return path, 'check-failed: %s' % (e,), None, 0.0
    tilt = float(np.degrees(np.arccos(np.clip(n[..., 2], -1, 1))).mean())
    return path, 'generated', blp, tilt


def select(path):
    if not KEEP.search(path):
        return 'no-keyword'
    if DROP.search(path):
        return 'excluded-keyword'
    if METAL.search(path) and not re.search(r'stone|rock|brick', path, re.I):
        return 'metal'
    return None


def write_mpq(lib, path, files):
    if os.path.exists(path):
        os.remove(path)
    h = HANDLE()
    slots = 1 << max(10, (len(files) * 2).bit_length())
    if not lib.SFileCreateArchive(path.encode(), 0x01000000 | 0x00100000 | 0x00200000, slots, ctypes.byref(h)):
        sys.exit('cannot create ' + path)
    for name in sorted(files):
        data, hf = files[name], HANDLE()
        if not lib.SFileCreateFile(h, name.encode('latin-1'), ctypes.c_uint64(0), DWORD(len(data)), DWORD(0),
                                   DWORD(0x200 | 0x80000000), ctypes.byref(hf)):
            sys.exit('cannot add ' + name)
        if not lib.SFileWriteFile(hf, data, DWORD(len(data)), DWORD(0x02)):
            sys.exit('cannot write ' + name)
        lib.SFileFinishFile(hf)
    lib.SFileCompactArchive(h, None, 0)
    lib.SFileCloseArchive(h)


def selftest():
    # a raised bump in the middle: its right side must lean +u (R > 0.5), its lower side +v (G > 0.5)
    y, x = np.mgrid[0:64, 0:64]
    lum = np.exp(-((x - 32) ** 2 + (y - 32) ** 2) / 60.0)
    img = Image.fromarray((np.stack([lum] * 3, -1) * 200 + 20).astype(np.uint8))
    n = normal_map(img)
    assert n[32, 38, 0] > 0.05 and n[38, 32, 1] > 0.05 and n[32, 26, 0] < -0.05 and n[26, 32, 1] < -0.05
    dec = check_blp(blp_dxt1(n))
    assert dec[32, 38, 0] > 0 and dec[38, 32, 1] > 0
    print('selftest ok')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--client', default='/root/tswow-server/tswow-client')
    ap.add_argument('--out', default='/root/wmonormals-data')
    ap.add_argument('--retail-listfile', help='CASC listfile.csv; retail _n files there are reported (none exist)')
    ap.add_argument('-j', type=int, default=os.cpu_count())
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    t0 = time.time()
    arc = Archives(os.path.join(a.client, 'Data'))
    roots = sorted({p for n in STOCK for p in arc.listfile(n)
                    if p.lower().endswith('.wmo') and not re.search(r'_\d{3}\.wmo$', p, re.I)}, key=str.lower)
    textures = {}                                   # lower -> path as first found in MOTX
    parsed = 0
    for r in roots:
        d = arc.read(r)
        if d:
            parsed += 1
            for t in motx(d):
                textures.setdefault(t.lower(), t)
    print('roots %d parsed %d unique textures %d' % (len(roots), parsed, len(textures)))

    verdict, jobs = {}, []
    for low, path in sorted(textures.items()):
        v = select(path)
        if v:
            verdict[path] = v
            continue
        d = arc.read(path)
        if d is None:
            verdict[path] = 'missing'
        else:
            jobs.append((path, d))
    print('selected by path %d' % len(jobs))
    if a.retail_listfile:
        want = {re.sub(r'\.blp$', '_n.blp', p.lower().replace('\\', '/')) for p, _ in jobs}
        hits = [ln for ln in open(a.retail_listfile, encoding='latin-1')
                if ln.split(';', 1)[-1].strip().lower() in want]
        print('retail _n matches: %d' % len(hits))

    files, tilts = {}, []
    with Pool(a.j) as pool:
        for path, v, blp, tilt in pool.imap_unordered(process, jobs, chunksize=8):
            verdict[path] = v
            if blp:
                name = path[:-4] + '_n.blp'
                files[name] = blp
                tilts.append(tilt)
                fp = os.path.join(a.out, 'out', *name.split('\\'))
                os.makedirs(os.path.dirname(fp), exist_ok=True)
                with open(fp, 'wb') as fh:
                    fh.write(blp)
    write_mpq(arc.lib, os.path.join(a.out, 'wmonormals.MPQ'), files)
    with open(os.path.join(a.out, 'report.tsv'), 'w') as fh:
        for p in sorted(verdict, key=str.lower):
            fh.write('%s\t%s\n' % (p, verdict[p]))
    counts = {}
    for v in verdict.values():
        counts[v.split(':')[0]] = counts.get(v.split(':')[0], 0) + 1
    print('verdicts', counts)
    if tilts:
        print('mean tilt %.1f deg (per-texture min %.1f max %.1f)' % (np.mean(tilts), min(tilts), max(tilts)))
    print('generated %d, %.0fs' % (len(files), time.time() - t0))


if __name__ == '__main__':
    main()
