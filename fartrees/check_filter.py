#!/usr/bin/env python3
"""Check of FarTreesBake's tree filter and card geometry (the wxl-pack billboard port, FARTREES-PLAN.md section 9).

    python3 -I fartrees/check_filter.py [listfile] [client Data dir]

1. Runs IsTree (FarTreesBake.cpp, copied 1:1 below) over every .m2 of the listfile, asserts the known non-trees
   are out and known trees in, and prints the tree-named paths the extra exclusions drop.
2. For the sample models (read from the client's MPQs with /root/mpq/mpqextract, as FarTreesBake reads them through
   SFile) prints Impostor.cpp's frame = max(zHi - zLo, 2 x reach) and bottom = zLo over every vertex, the header
   height and whether the 0.5 - 500 yd header test and the 6 yd placed test (at scale 1) pass.
Keep IsTree / frame() here in step with FarTreesBake.cpp.
"""
import os, struct, subprocess, sys, tempfile

LISTFILE = sys.argv[1] if len(sys.argv) > 1 else '/root/epoch_complete_listfile.txt'
DATA = sys.argv[2] if len(sys.argv) > 2 else '/root/tswow-server/tswow-client/Data'
ORDER = ['custom', 'patch-Y', 'patch-V', 'patch-T', 'patch-S', 'patch-R', 'patch-G', 'patch-F', 'patch-E', 'patch-D',
         'patch-C', 'patch-B', 'patch-A', 'patch-3', 'patch-2', 'patch', 'lichking', 'expansion', 'common-2', 'common']


def is_tree(p):   # FarTreesBake.cpp IsTree, on the lower-case path
    has = lambda w: w in p
    treeish = has('tree') or has('palm') or has('kalidarbush') or has('felwoodbush')
    if not treeish or has('street') or (has('leaves') and not has('noleaves')) or has('stump') or has('log') \
            or (has('root') and not has('treeroot')):
        return False
    return not any(has(w) for w in ('banner', 'hut', 'bridge', 'dock', 'cloak', 'spine'))


def wxl_tree(p):  # the same without our extra exclusions (wraithhost-lite FarLod.cpp ModelInfo)
    has = lambda w: w in p
    return (has('tree') or has('palm') or has('kalidarbush') or has('felwoodbush')) and not (
        has('street') or (has('leaves') and not has('noleaves')) or has('stump') or has('log')
        or (has('root') and not has('treeroot')))


def read(path):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'f')
        for a in ORDER:
            mpq = os.path.join(DATA, a + '.MPQ')
            if os.path.isfile(mpq) and subprocess.run(['/root/mpq/mpqextract', mpq, 'get', path, out],
                                                      capture_output=True).returncode == 0 and os.path.getsize(out):
                return open(out, 'rb').read()
    return None


def frame(m):     # Impostor.cpp Render: frame and bottom over every vertex; header box height
    nv, ov = struct.unpack_from('<II', m, 0x3C)
    zs, reach = [], 0.0
    for i in range(nv):
        x, y, z = struct.unpack_from('<3f', m, ov + 48 * i)
        zs.append(z)
        reach = max(reach, (x * x + y * y) ** 0.5)
    box = struct.unpack_from('<6f', m, 0xA0)
    return max(max(zs) - min(zs), 2 * reach), min(zs), box[5] - box[2], max(box[3] - box[0], box[4] - box[1])


paths = [l.strip().replace('/', '\\').lower() for l in open(LISTFILE, errors='replace') if l.strip().lower().endswith('.m2')]
trees = [p for p in paths if is_tree(p)]
dropped = [p for p in paths if wxl_tree(p) and not is_tree(p)]
print('%d .m2, %d trees by the filter; the extra exclusions drop %d:' % (len(paths), len(trees), len(dropped)))
for p in dropped:
    print('   ', p)
for leaf in ('battlegladespinehumandark', 'blastedlandsspine01', 'snakespinegiant01', 'hangingcloakred', 'nightelftreebanner02',
             'razorfen_canopy01_hole', 'silverpinedocks02', 'losttreehuts01', 'sholazarpalm_bridge01', 'treestump', 'fallenleaves'):
    assert not any(p.endswith('\\' + leaf + '.m2') and is_tree(p) for p in paths), leaf
SAMPLES = [r'world\azeroth\elwynn\passivedoodads\trees\elwynntreecanopy01.m2',
           r'world\lordaeron\silverpine\passivedoodads\trees\silverpinetree04.m2',
           r'world\kalimdor\darkshore\passivedoodads\darkshoretrees\darkshoretree01.m2',
           r'world\kalimdor\kalidar\passivedoodads\kalidartrees\kalidartree01.m2']
for s in SAMPLES:
    assert is_tree(s), s
gz = [p for p in paths if p.endswith('grizzlyhills_tree02.m2')]
print('\nmodel: frame, bottom, header height / width, header 0.5-500 ok, placed >= 6 yd at scale 1')
for s in SAMPLES + gz:
    m = read(s)
    if not m:
        print('   %s: not in the client MPQs' % s)
        continue
    f, b, h, w = frame(m)
    assert f >= h - 1e-3 and f > 0.1
    print('   %-45s frame %6.2f bottom %6.2f height %6.2f width %6.2f  %s %s' % (
        s.split('\\')[-1], f, b, h, w, 0.5 < h <= 500 and w > 0.1, h >= 6.0))
print('ok')
