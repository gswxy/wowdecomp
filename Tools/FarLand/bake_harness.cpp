// Measures farLand's tile bake (WoW/Source/WorldClient/FarLandBake.cpp) natively, reading a client's MPQs with
// StormLib the way the worker reads them through SFile (patches first).
//
//   clang++ -O2 -std=c++17 -I. Tools/FarLand/bake_harness.cpp WoW/Source/WorldClient/FarLandBake.cpp -lstorm -o /tmp/flh
//   /tmp/flh <client>/Data <Map> [tiles] [out.bin] [first tile, row * 64 + col]
//
// Prints ms a tile (ADT read + CRC, bake) over the first [tiles] tiles of the map's WDT (default 50); out.bin gets
// the first tile's FarLandBake::Tile for a look.
#include <StormLib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <memory>
#include <string>
#include <vector>
#include "WoW/Source/WorldClient/FarLandBake.h"

static std::vector<HANDLE> s_mpqs;

static bool Read(const std::string& path, std::vector<uint8_t>& out) {
    for (HANDLE h : s_mpqs) {
        HANDLE f;
        if (!SFileOpenFileEx(h, path.c_str(), 0, &f)) continue;
        const DWORD size = SFileGetFileSize(f, nullptr);
        out.resize(size);
        DWORD got = 0;
        SFileReadFile(f, out.data(), size, &got, nullptr);
        SFileCloseFile(f);
        out.resize(got);
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <Data> <Map> [tiles] [out.bin]\n", argv[0]); return 2; }
    const std::string data = argv[1], map = argv[2];
    const int want = argc > 3 ? std::atoi(argv[3]) : 50;
    std::vector<std::string> names;
    if (DIR* d = opendir(data.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("patch", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".MPQ") names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.rbegin(), names.rend());
    for (const char* n : { "lichking.MPQ", "expansion.MPQ", "common-2.MPQ", "common.MPQ", "enUS/patch-enUS-3.MPQ", "enUS/patch-enUS-2.MPQ",
                           "enUS/patch-enUS.MPQ", "enUS/lichking-locale-enUS.MPQ", "enUS/locale-enUS.MPQ" }) names.push_back(n);
    for (auto& n : names) {
        HANDLE h;
        if (SFileOpenArchive((data + "/" + n).c_str(), 0, STREAM_FLAG_READ_ONLY, &h)) s_mpqs.push_back(h);
    }

    FarLandBake::Context ctx;
    ctx.read = Read;
    std::vector<uint8_t> wdt, adt;
    if (!Read("World\\Maps\\" + map + "\\" + map + ".wdt", wdt)) { std::fprintf(stderr, "no WDT\n"); return 1; }
    ctx.bigAlpha = FarLandBake::ReadBigAlpha(wdt);
    size_t mainAt = 0;
    for (size_t i = 0; i + 8 <= wdt.size(); i += 8 + *reinterpret_cast<uint32_t*>(&wdt[i + 4]))
        if (std::string(reinterpret_cast<char*>(&wdt[i]), 4) == "NIAM") { mainAt = i + 8; break; }
    auto tile = std::make_unique<FarLandBake::Tile>();
    double readMs = 0, bakeMs = 0, worst = 0;
    int done = 0;
    for (int t = argc > 5 ? std::atoi(argv[5]) : 0; t < 4096 && done < want; t++) {
        if (!(*reinterpret_cast<uint32_t*>(&wdt[mainAt + 8 * t]) & 1)) continue;
        char path[256];
        std::snprintf(path, sizeof(path), "World\\Maps\\%s\\%s_%d_%d.adt", map.c_str(), map.c_str(), t % 64, t / 64);
        auto t0 = std::chrono::steady_clock::now();
        if (!Read(path, adt)) continue;
        volatile uint32_t crc = FarLandBake::Crc32(adt.data(), adt.size());
        (void)crc;
        auto t1 = std::chrono::steady_clock::now();
        if (!FarLandBake::Bake(ctx, adt.data(), adt.size(), *tile)) { std::printf("%s: bake failed\n", path); continue; }
        auto t2 = std::chrono::steady_clock::now();
        const double b = std::chrono::duration<double, std::milli>(t2 - t1).count();
        readMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        bakeMs += b;
        worst = std::max(worst, b);
        if (done == 0 && argc > 4) {
            FILE* f = std::fopen(argv[4], "wb");
            std::fwrite(tile.get(), sizeof(*tile), 1, f);
            std::fclose(f);
            std::printf("first: %s mean %u %u %u\n", path, tile->mean[0], tile->mean[1], tile->mean[2]);
        }
        done++;
    }
    std::printf("%s: %d tiles, read+crc %.2f ms/tile, bake %.2f ms/tile (worst %.2f), %zu textures cached\n", map.c_str(), done,
                readMs / std::max(done, 1), bakeMs / std::max(done, 1), worst, ctx.texMean.size());
    return 0;
}
