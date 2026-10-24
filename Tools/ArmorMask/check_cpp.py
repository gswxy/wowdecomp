"""Checks WoW/Common/ComponentCore/ArmorMask.cpp's ArmorMaskClassify against classify.py on real item textures:
builds the C++ classifier natively with g++ (the engine parts left out) and compares byte for byte.
    python3 Tools/ArmorMask/check_cpp.py        (needs the client MPQs, see mpq.py, and libstorm)"""
import os, re, subprocess, sys, tempfile
import numpy as np
import compose, classify

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, '..', '..', 'WoW', 'Common', 'ComponentCore', 'ArmorMask.cpp')).read()
helpers = src[src.index('namespace {'):src.index('// mask id -> texels')] + '}\n'
fn = re.search(r'^void ArmorMaskClassify.*?^}\n', src, re.S | re.M).group(0)
main = r'''
int main(int, char** v) {
    int w, h; FILE* f = fopen(v[1], "rb"); if (fread(&w, 4, 1, f) + fread(&h, 4, 1, f) != 2) return 1;
    std::vector<uint8_t> px(4 * w * h); if (fread(px.data(), 1, px.size(), f) != px.size()) return 1; fclose(f);
    std::vector<float> m(w * h), r(w * h); ArmorMaskClassify(px.data(), w, h, m.data(), r.data());
    f = fopen(v[2], "wb"); fwrite(m.data(), 4, w * h, f); fwrite(r.data(), 4, w * h, f); fclose(f);
}
'''
tmp = tempfile.mkdtemp()
open(os.path.join(tmp, 'h.cpp'), 'w').write('#include <algorithm>\n#include <cmath>\n#include <cstdint>\n#include <cstdio>\n#include <vector>\n' + helpers + fn + main)
subprocess.run(['g++', '-O2', '-std=c++17', '-o', os.path.join(tmp, 'h'), os.path.join(tmp, 'h.cpp')], check=True)
worst = 0
for fold, d, fi in [('TorsoUpperTexture', 33635, 18), ('TorsoUpperTexture', 31505, 18), ('LegUpperTexture', 33637, 20), ('TorsoUpperTexture', 35415, 18)]:
    u8 = np.clip(np.round(compose.nearest(compose.comp_tex(compose.S(compose.idi[d][fi]), fold), 128, 64) * 255), 0, 255).astype(np.uint8)
    open(os.path.join(tmp, 'in'), 'wb').write(np.array([128, 64], np.int32).tobytes() + u8[..., [2, 1, 0, 3]].tobytes())
    subprocess.run([os.path.join(tmp, 'h'), os.path.join(tmp, 'in'), os.path.join(tmp, 'out')], check=True)
    o = np.fromfile(os.path.join(tmp, 'out'), np.float32).reshape(2, 64, 128)
    pm, pr = classify.classify(u8.astype(np.float32) / 255)
    worst = max(worst, np.abs(np.round(o[0] * 255) - np.round(pm * 255)).max(), np.abs(np.round(o[1] * 255) - np.round(pr * 255)).max())
assert worst <= 1, 'C++ and Python classifiers differ by %d/255' % worst
print('ArmorMaskClassify matches classify.py (max byte diff %d)' % worst)
