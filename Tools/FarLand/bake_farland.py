"""Offline bake for farLand (WoW/Source/WorldClient/FarLand.cpp): per map, the real ground colour and the
ground's normals of every ADT tile, packed into two atlases the client draws the WDL horizon with.

    python3 -I Tools/FarLand/bake_farland.py --data <client>/Data [--out Tools/CustomMpq/FarLand] [Map ...]

Default maps: Azeroth Kalimdor Expansion01 Northrend. Output per map (packed by build_custom_mpq.py as
FarLand\\<Map>.*, generated, not committed):
  <Map>.flnd     'FLND', u32 version 1, u16 slotsX, slotsY, albedo slot px, normal slot px,
                 i16[4096] slot of tile (row * 64 + col, as CMapLowDetail::m_areas), -1 = no tile.
  <Map>_a.blp    albedo atlas, DXT1, 128 px a tile (4.17 yd a texel, one MCLY texture repeat).
  <Map>_n.blp    normal atlas, DXT5nm (world normal x in alpha, y in green; z = sqrt(1 - x2 - y2)), 64 px a tile.

Texel (u, v) of a slot: u along the tile's columns (world -Y), v along its rows (world -X), cell centred, so the
client maps a tile edge to the edge texel's centre. Colour: each MCLY layer's texture reduced to its mean (its
4 x 4 mip averaged: at 4.17 yd a texel one repeat is one texel, what the real terrain's mips show there),
blended by MCAL (sizeAlpha-sized: never the MCAL header), big alpha = weighted sum, else layered lerp,
x MCCV (x 2), liquids (MH2O) in a flat liquid colour with an upward normal. Normals from MCVT heights,
central differences over 8.33 yd.

Reads the client MPQs read-only with StormLib (build_custom_mpq.storm). Standard library + numpy.
"""
import argparse
import ctypes
import glob
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'CustomMpq'))
import build_custom_mpq as bm  # noqa: E402

TILE = 1600.0 / 3.0
SA, SN = 128, 64                       # albedo / normal slot px
LIQUID = {0: (0.10, 0.17, 0.20), 1: (0.07, 0.13, 0.18), 2: (0.85, 0.30, 0.05), 3: (0.25, 0.45, 0.10)}


class Mpq:
    def __init__(self, data):
        self.lib = bm.storm()
        pats = sorted((os.path.basename(p) for p in glob.glob(os.path.join(data, 'patch*.MPQ'))), reverse=True)
        self.h = []
        loc = sorted((os.path.join('enUS', os.path.basename(p)) for p in glob.glob(os.path.join(data, 'enUS', 'patch-enUS*.MPQ'))),
                     reverse=True) + [os.path.join('enUS', n) for n in ('lichking-locale-enUS.MPQ', 'expansion-locale-enUS.MPQ', 'locale-enUS.MPQ')]
        for n in pats + ['lichking.MPQ', 'expansion.MPQ', 'common-2.MPQ', 'common.MPQ'] + loc:   # locale last: DBCs only
            h = bm.HANDLE()
            if self.lib.SFileOpenArchive(os.path.join(data, n).encode(), 0, bm.MPQ_OPEN_READ_ONLY, ctypes.byref(h)):
                self.h.append(h)

    def read(self, path):
        for h in self.h:
            hf = bm.HANDLE()
            if self.lib.SFileOpenFileEx(h, path.encode(), 0, ctypes.byref(hf)):
                size = self.lib.SFileGetFileSize(hf, None)
                buf = ctypes.create_string_buffer(size)
                got = bm.DWORD()
                self.lib.SFileReadFile(hf, buf, size, ctypes.byref(got), None)
                self.lib.SFileCloseFile(hf)
                return buf.raw[:got.value]
        return None


def chunks(d, start=0, end=None):
    out, i, end = {}, start, len(d) if end is None else end
    while i + 8 <= end:
        tag, size = d[i:i + 4][::-1].decode('latin1'), struct.unpack_from('<I', d, i + 4)[0]
        out.setdefault(tag, []).append((i + 8, size))
        i += 8 + size
    return out


def rgb565(c):
    return np.stack([(c >> 11) & 31, (c >> 5) & 63, c & 31], -1) * np.array([255 / 31, 255 / 63, 255 / 31])


def dxt_colour_block(b):
    c0, c1 = struct.unpack_from('<HH', b, 0)
    p = rgb565(np.array([c0, c1]))
    pal = [p[0], p[1], (2 * p[0] + p[1]) / 3, (p[0] + 2 * p[1]) / 3] if c0 > c1 else [p[0], p[1], (p[0] + p[1]) / 2, p[0] * 0]
    idx = struct.unpack_from('<I', b, 4)[0]
    return np.array([pal[(idx >> (2 * k)) & 3] for k in range(16)])


_texcache = {}


def tex_mean(mpq, name):
    """Mean rgb (0..1) of a BLP2's 4 x 4 mip (or its smallest)."""
    key = name.lower()
    if key in _texcache:
        return _texcache[key]
    d, col = mpq.read(name), np.array([0.5, 0.5, 0.5])
    if d and d[:4] == b'BLP2':
        enc, adepth, aenc, _ = d[8:12]
        w, h = struct.unpack_from('<II', d, 12)
        offs, sizes = struct.unpack_from('<16I', d, 20), struct.unpack_from('<16I', d, 84)
        lv = 0
        while lv + 1 < 16 and sizes[lv + 1] and max(w >> (lv + 1), 1) >= 4 and max(h >> (lv + 1), 1) >= 4:
            lv += 1
        o, mw, mh = offs[lv], max(w >> lv, 1), max(h >> lv, 1)
        if enc == 2:
            step = 8 if aenc == 0 else 16
            blocks = max(mw // 4, 1) * max(mh // 4, 1)
            col = np.mean([dxt_colour_block(d[o + k * step + step - 8:o + k * step + step]) for k in range(blocks)], (0, 1)) / 255
        elif enc == 1:
            pal = np.frombuffer(d, np.uint8, 1024, 148).reshape(256, 4)[:, 2::-1]
            col = pal[np.frombuffer(d, np.uint8, mw * mh, o)].mean(0) / 255
        elif enc == 3:
            col = np.frombuffer(d, np.uint8, mw * mh * 4, o).reshape(-1, 4)[:, 2::-1].mean(0) / 255
    _texcache[key] = col
    return col


def alpha_maps(d, mcal, ly, n, big, fix):
    """64 x 64 float alpha for layers 1..n-1 (layer table ly: (texId, flags, ofsAlpha))."""
    out = []
    for k in range(1, n):
        _, flags, ofs = ly[k]
        p = mcal + ofs
        if big and flags & 0x200:
            a, i = bytearray(), p
            while len(a) < 4096:
                b = d[i]; i += 1
                if b & 0x80:
                    a += bytes([d[i]]) * (b & 0x7f); i += 1
                else:
                    a += d[i:i + (b & 0x7f)]; i += b & 0x7f
            m = np.frombuffer(bytes(a[:4096]), np.uint8).reshape(64, 64) / 255.0
        elif big:
            m = np.frombuffer(d, np.uint8, 4096, p).reshape(64, 64) / 255.0
        else:
            q = np.frombuffer(d, np.uint8, 2048, p)
            m = np.stack([q & 15, q >> 4], -1).reshape(64, 64) / 15.0
            if not fix:   # last row / column are not stored meaningfully: repeat their neighbours
                m[:, 63] = m[:, 62]; m[63, :] = m[62, :]
        out.append(m)
    return out


def liquid_mask(d, mh2o, ltype):
    """(16, 16, 8, 8) liquid category per chunk cell, -1 = dry."""
    res = -np.ones((16, 16, 8, 8), np.int8)
    if mh2o is None:
        return res
    base = mh2o
    for c in range(256):
        oi, cnt, _ = struct.unpack_from('<III', d, base + c * 12)
        for k in range(cnt if oi else 0):
            t, _, _, _, x, y, w, h, oe, _ = struct.unpack_from('<HHffBBBBII', d, base + oi + k * 24)
            bits = int.from_bytes(d[base + oe:base + oe + 8], 'little') if oe else (1 << 64) - 1
            j = 0
            for r in range(y, y + h):
                for q in range(x, x + w):
                    if bits >> j & 1 and r < 8 and q < 8:
                        res[c // 16, c % 16, r, q] = ltype.get(t, 0)
                    j += 1
    return res


def bake_tile(d, mpq, big):
    top = chunks(d)
    tex = [s.decode('latin1') for s in d[top['MTEX'][0][0]:sum(top['MTEX'][0])].split(b'\0') if s]
    means = np.array([tex_mean(mpq, t) for t in tex]) if tex else np.zeros((1, 3))
    col = np.zeros((128, 128, 3))
    hgt = np.zeros((129, 129))
    mh2o = top['MH2O'][0][0] if 'MH2O' in top else None
    for p, size in top.get('MCNK', []):
        flags, ix, iy, nl = struct.unpack_from('<IIII', d, p)
        o_h, _, o_ly = struct.unpack_from('<III', d, p + 0x14)
        o_al, sz_al = struct.unpack_from('<II', d, p + 0x24)
        z0 = struct.unpack_from('<f', d, p + 0x70)[0]
        o_cv = struct.unpack_from('<I', d, p + 0x74)[0]
        c0 = p - 8
        v = np.frombuffer(d, '<f4', 145, c0 + o_h + 8).astype(np.float64) + z0
        outer = np.concatenate([v[r * 17:r * 17 + 9] for r in range(9)]).reshape(9, 9)
        hgt[iy * 8:iy * 8 + 9, ix * 8:ix * 8 + 9] = outer
        ly = [struct.unpack_from('<III', d, c0 + o_ly + 8 + 16 * k) for k in range(nl)]
        if nl:
            c = np.broadcast_to(means[min(ly[0][0], len(means) - 1)], (8, 8, 3)).copy()
            if nl > 1 and sz_al > 8:
                al = alpha_maps(d, c0 + o_al + 8, ly, nl, big, flags & 0x8000)
                al = [a.reshape(8, 8, 8, 8).mean((1, 3))[..., None] for a in al]   # one value a texel
                if big:
                    c = c * np.clip(1 - sum(al), 0, 1)
                    for k, a in enumerate(al):
                        c = c + a * means[min(ly[k + 1][0], len(means) - 1)]
                else:
                    for k, a in enumerate(al):
                        c = c + (means[min(ly[k + 1][0], len(means) - 1)] - c) * a
        else:
            c = np.full((8, 8, 3), 0.5)
        if o_cv and flags & 0x40:
            cv = np.frombuffer(d, np.uint8, 145 * 4, c0 + o_cv + 8).reshape(145, 4)[:, 2::-1] / 127.5
            ov = np.concatenate([cv[r * 17:r * 17 + 9] for r in range(9)]).reshape(9, 9, 3)
            # bilinear at the 8 x 8 texel centres
            ov = (ov[:-1, :-1] + ov[1:, :-1] + ov[:-1, 1:] + ov[1:, 1:]) * 0.25
            c = c * ov
        col[iy * 8:iy * 8 + 8, ix * 8:ix * 8 + 8] = c
    return col, hgt, mh2o


def normals(hgt, wet):
    s = TILE / 128
    r = np.arange(1, 128, 2)
    nx = (hgt[r + 1][:, r] - hgt[r - 1][:, r]) / (2 * s)
    ny = (hgt[r][:, r + 1] - hgt[r][:, r - 1]) / (2 * s)
    n = np.stack([nx, ny, np.ones_like(nx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    n[wet] = (0, 0, 1)
    return n


# ---------- DXT encoders (range fit, numpy) ----------
def enc565(c):
    q = np.clip(np.rint(c * [31 / 255, 63 / 255, 31 / 255]), 0, [31, 63, 31]).astype(np.uint32)
    return (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]


def dxt_colour(blk):
    """blk (N, 16, 3) 0..255 -> (N, 8) uint8, 4-colour mode."""
    c = blk - blk.mean(1, keepdims=True)
    cov = np.einsum('nki,nkj->nij', c, c)
    ax = np.ones((len(blk), 3), blk.dtype)
    for _ in range(4):   # power iteration: the block's main colour axis
        ax = np.einsum('nij,nj->ni', cov, ax)
        ax /= np.maximum(np.abs(ax).max(1, keepdims=True), 1e-9)
    pr = np.einsum('nki,ni->nk', c, ax)
    n = np.arange(len(blk))
    hi, lo = enc565(blk[n, pr.argmax(1)]), enc565(blk[n, pr.argmin(1)])
    hi, lo = np.maximum(hi, lo), np.minimum(hi, lo)
    p0, p1 = rgb565(hi), rgb565(lo)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    idx = ((blk[:, :, None, :] - pal[:, None]) ** 2).sum(-1).argmin(-1).astype(np.uint32)
    idx[hi == lo] = 0
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((len(blk), 8), np.uint8)
    out[:, 0:2] = hi.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = lo.astype('<u2').view(np.uint8).reshape(-1, 2)
    out[:, 4:8] = bits.astype('<u4').view(np.uint8).reshape(-1, 4)
    return out


def dxt_alpha(a):
    """a (N, 16) 0..255 -> (N, 8) DXT5 alpha blocks, 8-level mode."""
    a0, a1 = a.max(1).astype(np.int64), a.min(1).astype(np.int64)
    span = np.maximum(a0 - a1, 1)[:, None]
    k = np.clip(np.rint((a - a1[:, None]) * 7 / span), 0, 7).astype(np.int64)
    code = np.where(k == 7, 0, np.where(k == 0, 1, 8 - k))
    code[a0 == a1] = 0
    bits = (code.astype(np.uint64) << (3 * np.arange(16, dtype=np.uint64))).sum(1)
    out = np.zeros((len(a), 8), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    out[:, 2:8] = bits.astype('<u8').view(np.uint8).reshape(-1, 8)[:, :6]
    return out


def blocks(img):
    h, w, ch = img.shape
    ph, pw = max(h, 4), max(w, 4)
    if (ph, pw) != (h, w):
        img = np.pad(img, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    return img.reshape(ph // 4, 4, pw // 4, 4, ch).transpose(0, 2, 1, 3, 4).reshape(-1, 16, ch)


def mips(img):
    out = [img]
    while img.shape[0] > 1 or img.shape[1] > 1:
        h, w = max(img.shape[0] // 2, 1), max(img.shape[1] // 2, 1)
        img = img[:h * 2 if img.shape[0] > 1 else 1, :w * 2 if img.shape[1] > 1 else 1]
        img = img.reshape(h, img.shape[0] // h, w, img.shape[1] // w, -1).mean((1, 3))
        out.append(img)
    return out


def write_blp(path, img, dxt5):
    """img (H, W, 4) float 0..255 rgba. DXT1 (rgb) or DXT5 (rgba)."""
    datas = []
    for m in mips(img)[:16]:
        part = []
        for y in range(0, max(m.shape[0], 4), 64):   # 16 block rows a strip: bounded memory
            b = blocks(m[y:y + 64])
            if dxt5:
                part.append(np.concatenate([dxt_alpha(b[..., 3]), dxt_colour(b[..., :3])], 1).tobytes())
            else:
                part.append(dxt_colour(b[..., :3]).tobytes())
        datas.append(b''.join(part))
    h, w = img.shape[:2]
    head = b'BLP2' + struct.pack('<I', 1) + bytes([2, 8 if dxt5 else 0, 7 if dxt5 else 0, 1]) + struct.pack('<II', w, h)
    offs, sizes, o = [], [], 20 + 128 + 1024
    for dd in datas:
        offs.append(o); sizes.append(len(dd)); o += len(dd)
    offs += [0] * (16 - len(offs)); sizes += [0] * (16 - len(sizes))
    with open(path, 'wb') as f:
        f.write(head + struct.pack('<16I', *offs) + struct.pack('<16I', *sizes) + bytes(1024) + b''.join(datas))


def liquid_types(mpq):
    d = mpq.read('DBFilesClient\\LiquidType.dbc')
    if not d:
        return {}
    n, nf, rs = struct.unpack_from('<III', d, 4)
    return {struct.unpack_from('<I', d, 20 + i * rs)[0]: struct.unpack_from('<I', d, 20 + i * rs + 12)[0] for i in range(n)}


def bake_map(mpq, name, outdir, ltype):
    wdt = mpq.read('World\\Maps\\%s\\%s.wdt' % (name, name))
    if not wdt:
        print('%s: no WDT' % name)
        return
    c = chunks(wdt)
    big = bool(struct.unpack_from('<I', wdt, c['MPHD'][0][0])[0] & 0x4)
    main = c['MAIN'][0][0]
    tiles = [i for i in range(4096) if struct.unpack_from('<I', wdt, main + 8 * i)[0] & 1]
    if not tiles:
        print('%s: no ADT tiles (WMO-only map)' % name)
        return
    sx = 32 if len(tiles) <= 1024 else 64
    sy = 32
    alb = np.zeros((sy * SA, sx * SA, 4), np.float32)
    nrm = np.zeros((sy * SN, sx * SN, 4), np.float32)
    nrm[..., 1] = nrm[..., 3] = 127.5
    slot = [-1] * 4096
    for k, t in enumerate(tiles):
        r, q = divmod(t, 64)
        d = mpq.read('World\\Maps\\%s\\%s_%d_%d.adt' % (name, name, q, r))
        if not d:
            continue
        try:
            col, hgt, mh2o = bake_tile(d, mpq, big)
        except (struct.error, ValueError, IndexError, KeyError) as e:
            print('%s_%d_%d: skipped (%s)' % (name, q, r, e))
            continue
        liq = liquid_mask(d, mh2o, ltype)                                    # (16,16,8,8)
        liq = liq.transpose(0, 2, 1, 3).reshape(128, 128)
        for cat, rgb in LIQUID.items():
            col[liq == cat] = rgb
        wet = liq.reshape(64, 2, 64, 2).max((1, 3)) >= 0
        n = normals(hgt, wet)
        s = len([x for x in slot if x >= 0])
        slot[t] = s
        oy, ox = divmod(s, sx)
        alb[oy * SA:(oy + 1) * SA, ox * SA:(ox + 1) * SA, :3] = np.clip(col, 0, 1) * 255
        alb[oy * SA:(oy + 1) * SA, ox * SA:(ox + 1) * SA, 3] = 255
        nrm[oy * SN:(oy + 1) * SN, ox * SN:(ox + 1) * SN, 1] = (n[..., 1] * 0.5 + 0.5) * 255
        nrm[oy * SN:(oy + 1) * SN, ox * SN:(ox + 1) * SN, 3] = (n[..., 0] * 0.5 + 0.5) * 255
        if k % 100 == 0:
            print('%s: %d / %d tiles' % (name, k, len(tiles)), flush=True)
    os.makedirs(outdir, exist_ok=True)
    write_blp(os.path.join(outdir, name + '_a.blp'), alb, False)
    write_blp(os.path.join(outdir, name + '_n.blp'), nrm, True)
    with open(os.path.join(outdir, name + '.flnd'), 'wb') as f:
        f.write(b'FLND' + struct.pack('<I4H', 1, sx, sy, SA, SN) + struct.pack('<4096h', *slot))
    print('%s: %d tiles, atlas %dx%d' % (name, sum(1 for x in slot if x >= 0), sx * SA, sy * SA))


def selftest():
    rng = np.random.default_rng(1)
    t = rng.uniform(0, 1, (5, 16, 1))
    blk = rng.uniform(0, 255, (5, 1, 3)) * t + rng.uniform(0, 255, (5, 1, 3)) * (1 - t)   # gradients, as ground is
    blk[0] = 40
    enc = dxt_colour(blk)
    for i in range(5):
        dec = dxt_colour_block(enc[i].tobytes())
        assert np.abs(dec - blk[i]).max() < (8 if i == 0 else 48), (i, np.abs(dec - blk[i]).max())
    a = np.linspace(0, 255, 16)[None]
    e = dxt_alpha(a)[0]
    lv = [e[0], e[1]] + [((8 - c) * int(e[0]) + (c - 1) * int(e[1])) / 7 for c in range(2, 8)]
    bits = int.from_bytes(e[2:8].tobytes(), 'little')
    dec = [lv[bits >> (3 * k) & 7] for k in range(16)]
    assert max(abs(dec[k] - a[0, k]) for k in range(16)) <= 19, dec
    h = np.fromfunction(lambda r, c: r * 2.0, (129, 129))   # rising along rows (world -X): faces +X
    n = normals(h, np.zeros((64, 64), bool))
    assert n[10, 10, 0] > 0.3 and abs(n[10, 10, 1]) < 1e-9
    print('selftest ok')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--data', default='/root/tswow-server/tswow-client/Data')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(HERE), 'CustomMpq', 'FarLand'))
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('maps', nargs='*', default=['Azeroth', 'Kalimdor', 'Expansion01', 'Northrend'])
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    mpq = Mpq(a.data)
    ltype = liquid_types(mpq)
    for m in a.maps:
        bake_map(mpq, m, a.out, ltype)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
