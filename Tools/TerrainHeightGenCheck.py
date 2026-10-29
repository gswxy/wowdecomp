#!/usr/bin/env python3
"""TerrainHeightGenCheck: WoW/Source/WorldClient/TerrainHeightGen.cpp (the run-time terrain height map) against the
Python it ports (wxl-light tools/make_heightmaps.py at f4d6271), on the same decoded tileset textures.

    Tools/TerrainHeightGenCheck.py [OUT_DIR]

Builds the .cpp natively with clang++ (it has no engine deps), feeds both the PIL-decoded diffuse, prints per
texture the kind each picked, mean / max abs height difference and the C++ ms, and writes runtime_vs_baked.png
(rows: diffuse | C++ | Python). Needs numpy, scipy, PIL and libstorm.so (as make_heightmaps.py does). Exits 1 when
a kind differs or a mean difference passes 0.03.
"""
import importlib.util, io, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXTURES = [
    'tileset\\elwynn\\elwynncobblestonebase.blp', 'tileset\\duskwood\\duskwoodcobblestone.blp',
    'tileset\\burningstepps\\burningsteppsbrick01.blp', 'tileset\\expansion01\\eversong\\swathsmallstones.blp',
    'tileset\\araithihighlands\\arathihighlandsbrickfloor.blp', 'tileset\\elwynn\\elwynngrassbase.blp',
    'tileset\\alteracmtns\\alteracgrassbase.blp', 'tileset\\araithihighlands\\arathihighlandsdirt.blp',
    'tileset\\darkshore\\darkshoresand.blp', 'tileset\\expansion02\\boreantundra\\bt_campsnowa.blp',
]
KINDS = {0: 'soft', 1: 'stones', 2: 'stones, bright joints'}
MAIN = r'''
#include "WoW/Source/WorldClient/TerrainHeightGen.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    // argv: in.bgra side name out.f32 (repeated)
    for (int i = 1; i + 3 < argc; i += 4) {
        uint32_t side = std::atoi(argv[i + 1]);
        std::vector<uint8_t> px(side * side * 4);
        FILE* f = std::fopen(argv[i], "rb");
        std::fread(px.data(), 1, px.size(), f);
        std::fclose(f);
        std::vector<float> out;
        uint32_t outSide = 0;
        auto t0 = std::chrono::steady_clock::now();
        int kind = TerrainHeightGen::Generate(px.data(), side, argv[i + 2], out, outSide);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        f = std::fopen(argv[i + 3], "wb");
        std::fwrite(out.data(), 4, out.size(), f);
        std::fclose(f);
        std::printf("%d %u %.2f\n", kind, outSide, ms);
    }
}
'''


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    py = os.path.join(out, 'make_heightmaps.py')
    open(py, 'wb').write(subprocess.check_output(['git', '-C', '/root/wxl-light', 'show', 'f4d6271:tools/make_heightmaps.py']))
    spec = importlib.util.spec_from_file_location('make_heightmaps', py)
    mh = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mh)

    main_cpp = os.path.join(out, 'main.cpp')
    exe = os.path.join(out, 'heightgen')
    open(main_cpp, 'w').write(MAIN)
    subprocess.check_call(['clang++', '-O2', '-std=c++17', '-I', REPO, main_cpp, os.path.join(REPO, 'WoW/Source/WorldClient/TerrainHeightGen.cpp'), '-o', exe])

    found = mh.sources()
    imgs, args = [], []
    for i, name in enumerate(TEXTURES):
        img = Image.open(io.BytesIO(found[name][1]()))
        img.load()
        rgba = np.asarray(img.convert('RGBA'))
        raw = os.path.join(out, '%02d.bgra' % i)
        open(raw, 'wb').write(rgba[:, :, [2, 1, 0, 3]].tobytes())
        imgs.append(img)
        args += [raw, str(img.width), found[name][0], os.path.join(out, '%02d.f32' % i)]
    lines = subprocess.check_output([exe] + args).decode().split('\n')

    bad, rows, times = 0, [], []
    print('%-44s %-22s %-22s %7s %7s %6s' % ('texture', 'C++', 'Python', 'mean', 'max', 'ms'))
    for i, name in enumerate(TEXTURES):
        kind, side, ms = lines[i].split()
        side = int(side)
        c = np.fromfile(os.path.join(out, '%02d.f32' % i), np.float32).reshape(side, side)
        p, pkind = mh.height(imgs[i], found[name][0])
        diff = np.abs(c - p)
        ok = KINDS[int(kind)] == pkind and diff.mean() < 0.03
        bad += not ok
        times.append(float(ms))
        print('%-44s %-22s %-22s %7.4f %7.4f %6s%s' % (name.rsplit('\\', 1)[-1], KINDS[int(kind)], pkind, diff.mean(), diff.max(), ms, '' if ok else '  FAIL'))
        tile = lambda a: Image.fromarray(np.clip(a * 255, 0, 255).astype(np.uint8)).convert('RGB').resize((128, 128))
        row = Image.new('RGB', (3 * 128, 128))
        row.paste(imgs[i].convert('RGB').resize((128, 128)), (0, 0))
        row.paste(tile(c), (128, 0))
        row.paste(tile(p), (256, 0))
        rows.append(row)
    sheet = Image.new('RGB', (3 * 128, 128 * len(rows)))
    for i, r in enumerate(rows):
        sheet.paste(r, (0, 128 * i))
    sheet.save(os.path.join(out, 'runtime_vs_baked.png'))
    print('C++ ms per texture: mean %.1f, max %.1f; sheet %s' % (np.mean(times), np.max(times), os.path.join(out, 'runtime_vs_baked.png')))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
