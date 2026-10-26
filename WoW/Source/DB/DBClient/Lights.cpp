#include <cstdint>
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/DBClient/ForeverSky.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include <vector>
#include "WoW/Source/DB/Rec/LightIntBandRec.h"
#include "WoW/Source/DB/Rec/LightParamsRec.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"

// VERIFY: 7c5384b3 manual
// OFFSET: 0x007EAE70
static void GetBandTimeRange(const int32_t* times, int32_t count, int32_t time, int32_t& index, float& t, int32_t& next) {
    next = 0;
    t = 0.0f;
    index = 0;

    if (count <= 0) {
        return;
    }

    do {
        int32_t start = times[index];
        next = (index + 1) % count;
        int32_t end = times[next];

        if (end > start) {
            if (time >= start && time <= end) {
                t = static_cast<float>(time - start) / (end - start);
                return;
            }
        } else if (time <= end || time >= start) {
            end += 2880;

            if (time < start) {
                time += 2880;
            }

            t = static_cast<float>(time - start) / (end - start);
            return;
        }

        index++;
    } while (index < count);
}

// VERIFY: ba90e0db manual
// OFFSET: 0x007EB070
static CImVector InterpolateIntBand(const LightIntBandRec& rec, int32_t time) {
    CImVector color;

    if (!rec.m_num) {
        *reinterpret_cast<uint32_t*>(&color) = 0xFF000000;
        return color;
    }

    int32_t index;
    float t;
    int32_t next;
    GetBandTimeRange(rec.m_time, rec.m_num, time, index, t, next);

    const CImVector& from = *reinterpret_cast<const CImVector*>(&rec.m_data[index]);
    const CImVector& to = *reinterpret_cast<const CImVector*>(&rec.m_data[next]);

    int32_t r = CMath::fint_((to.r - from.r) * t + from.r);
    int32_t g = CMath::fint_((to.g - from.g) * t + from.g);
    int32_t b = CMath::fint_((to.b - from.b) * t + from.b);

    *reinterpret_cast<uint32_t*>(&color) = ((((r & 0xFF) | 0xFFFFFF00) << 8 | (g & 0xFF)) << 8) | (b & 0xFF);
    return color;
}

// VERIFY: d2967a23 manual
// OFFSET: 0x007EAF60
static void TransformMapCoord(LightRec* rec, bool transform) {
    if ((rec->m_gameCoords[0] != 0.0f || rec->m_gameCoords[1] != 0.0f || rec->m_gameCoords[2] != 0.0f) && transform) {
        rec->m_gameCoords[0] *= 1.0f / 36.0f;
        rec->m_gameCoords[1] *= 1.0f / 36.0f;
        rec->m_gameCoords[2] *= 1.0f / 36.0f;
        rec->m_gameFalloffStart *= 1.0f / 36.0f;
        rec->m_gameFalloffEnd *= 1.0f / 36.0f;

        float offsetX = 0.0f;
        float offsetY = 0.0f;

        if (!World::MapIsDungeon()) {
            offsetX = 51200.0f / 3.0f;
            offsetY = 51200.0f / 3.0f;
        }

        float x = -(rec->m_gameCoords[0] - offsetY);
        float z = rec->m_gameCoords[1];
        rec->m_gameCoords[0] = -(rec->m_gameCoords[2] - offsetX);
        rec->m_gameCoords[1] = x;
        rec->m_gameCoords[2] = z;
    }
}

// VERIFY: c7d146ee auto
// OFFSET: 0x007EB010
static void TransformLights(bool transform) {
    for (int32_t i = 0; i < g_lightDB.m_numRecords; i++) {
        LightRec* rec = i >= 0 && i < g_lightDB.m_numRecords ? &g_lightDB.m_records[i] : nullptr;
        TransformMapCoord(rec, transform);
    }
}

// VERIFY: ae55f679 auto
// OFFSET: 0x007EB060
void InitLights(bool transform) {
    TransformLights(transform);
}

// VERIFY: b70039a6 auto
// OFFSET: 0x007EBF30
CImVector DayNight::LoadLightIntBand(int32_t time, const LightParamsRec* params, int32_t band) {
    LightIntBandRec rec;
    g_lightIntBandDB.GetRecord(band + 18 * params->m_id - 17, &rec);
    return InterpolateIntBand(rec, time);
}

// not stock: foreverSky = Wowhead Forever's own sky colours (VFog\ForeverSky.bin, ForeverSky.h) on the dome stops
// m_skyColors[0..4] (bands 2..6) of the params ids it lists, interpolated as the stock band, lerped stock -> Forever by
// strength (foreverSkyStrength). Band 7 (the fog colour, m_skyColors[5]) stays stock: the world fog and vfog's haze
// read it. Read once through SFile on first use; no or bad file = the stock sky.
void DayNight::ForeverSkyStops(int32_t time, const LightParamsRec* params, float strength, CImVector* sky) {
    static ForeverSky::Table s_table;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        SFileImpl* file = nullptr;
        SFileOpenFile("VFog\\ForeverSky.bin", &file);
        if (file) {
            std::vector<uint8_t> data(SFileGetFileSize(file, nullptr));
            const bool read = !data.empty() && SFileReadFile(file, data.data(), static_cast<uint32_t>(data.size()), nullptr, nullptr, 0);
            SFileCloseFile(file);
            if (!read || !ForeverSky::Parse(data.data(), data.size(), s_table)) {
                ConsolePrintf("foreverSky: VFog\\ForeverSky.bin is bad; stock sky");
            }
        } else {
            ConsolePrintf("foreverSky: no VFog\\ForeverSky.bin; stock sky");
        }
    }
    const auto it = params ? s_table.find(params->m_id) : s_table.end();
    if (it == s_table.end()) {
        return;
    }
    const ForeverSky::Keys& k = it->second;
    LightIntBandRec rec;
    rec.m_num = k.num;
    std::memcpy(rec.m_time, k.time, sizeof(rec.m_time));
    for (int32_t i = 0; i < 5; i++) {
        std::memcpy(rec.m_data, k.data[i], sizeof(rec.m_data));
        const CImVector f = InterpolateIntBand(rec, time);
        CImVector& c = sky[i];
        c.r = static_cast<uint8_t>(CMath::fint_(c.r + (f.r - c.r) * strength + 0.5f));
        c.g = static_cast<uint8_t>(CMath::fint_(c.g + (f.g - c.g) * strength + 0.5f));
        c.b = static_cast<uint8_t>(CMath::fint_(c.b + (f.b - c.b) * strength + 0.5f));
    }
}

// VERIFY: 95c2a7e6 auto
// OFFSET: 0x007EAEF0
float DayNight::InterpFloatBand(const LightFloatBandRec* rec, int32_t now, int32_t bandIndex) {
    if (!rec->m_num) {
        return 0.0f;
    }

    float t;
    int32_t lo;
    int32_t hi;
    GetBandTimeRange(rec->m_time, rec->m_num, now, lo, t, hi);

    if (bandIndex) {
        return (rec->m_data[hi] - rec->m_data[lo]) * t + rec->m_data[lo];
    }

    float loValue = rec->m_data[lo] * 0.027777778f;
    return loValue + (0.027777778f * rec->m_data[hi] - loValue) * t;
}

// VERIFY: fb110f5d manual
// OFFSET: 0x007ED2D0
void DayNight::ColorLerpBytes(CImVector* out, const CImVector* a, const CImVector* b, float t) {
    float blue = b->b < a->b ? a->b - (a->b - b->b) * t : a->b + (b->b - a->b) * t;
    float green = b->g < a->g ? a->g - (a->g - b->g) * t : a->g + (b->g - a->g) * t;
    float red = b->r < a->r ? a->r - (a->r - b->r) * t : a->r + (b->r - a->r) * t;
    out->b = CMath::fint_(blue - 0.5f);
    out->g = CMath::fint_(green - 0.5f);
    out->r = CMath::fint_(red - 0.5f);
    out->a = 0xFF;
}
