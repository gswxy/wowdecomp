#ifndef WOW_SOURCE_DB_DBCLIENT_FOREVERSKY_H
#define WOW_SOURCE_DB_DBCLIENT_FOREVERSKY_H

// not stock: foreverSky's table, VFog\ForeverSky.bin (Wowhead Forever 1.60.1 LightData SkyTop..SkyFog, keyed by our
// stock clear-weather LightParams id; baked by forever_sky/bake_forever_sky.py). Little endian:
//   'FSK1', u32 count, count x { u32 paramsId, u32 nKeys, nKeys x { u32 time (half-minutes), u32 colour[6] } }
// colour = 0x00RRGGBB in LightIntBand order, bands 2..7 = CurrentLight::m_skyColors[0..5].
// Header-only so the scratchpad check parses with the same code.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace ForeverSky {
struct Keys {
    int32_t num;
    int32_t time[16];
    int32_t data[6][16];
};

using Table = std::unordered_map<int32_t, Keys>;

// false (table cleared) on any malformed input; a params entry with more than 16 keys keeps its first 16.
inline bool Parse(const uint8_t* p, size_t size, Table& out) {
    out.clear();
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, p + o, 4); return v; };
    if (!p || size < 8 || std::memcmp(p, "FSK1", 4) != 0) return false;
    const uint32_t count = u32(4);
    size_t o = 8;
    for (uint32_t i = 0; i < count; ++i) {
        if (o + 8 > size) return out.clear(), false;
        const int32_t id = static_cast<int32_t>(u32(o));
        const uint32_t n = u32(o + 4);
        o += 8;
        if (n == 0 || n > (size - o) / 28) return out.clear(), false;
        Keys k = {};
        k.num = n < 16 ? static_cast<int32_t>(n) : 16;
        for (int32_t j = 0; j < k.num; ++j) {
            const size_t e = o + 28 * static_cast<size_t>(j);
            k.time[j] = static_cast<int32_t>(u32(e));
            for (int b = 0; b < 6; ++b) k.data[b][j] = static_cast<int32_t>(u32(e + 4 + 4 * b));
        }
        o += 28 * static_cast<size_t>(n);
        out[id] = k;
    }
    return true;
}
}

#endif
