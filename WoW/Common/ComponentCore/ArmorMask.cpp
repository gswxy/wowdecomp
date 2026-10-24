#include "WoW/Common/ComponentCore/ArmorMask.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/BLPFile/blp.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Services/Texture.h"

namespace {

float Ss(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// the mean of a over the (2r+1)^2 window clipped to the image, from its summed-area table s ((w+1) x (h+1))
void Box(const std::vector<double>& s, int32_t w, int32_t h, int32_t r, std::vector<float>& out) {
    out.resize(static_cast<size_t>(w) * h);

    for (int32_t y = 0; y < h; y++) {
        int32_t y0 = std::max(y - r, 0), y1 = std::min(y + r + 1, h);

        for (int32_t x = 0; x < w; x++) {
            int32_t x0 = std::max(x - r, 0), x1 = std::min(x + r + 1, w);
            double tot = s[y1 * (w + 1) + x1] - s[y0 * (w + 1) + x1] - s[y1 * (w + 1) + x0] + s[y0 * (w + 1) + x0];
            out[y * w + x] = static_cast<float>(tot / ((y1 - y0) * (x1 - x0)));
        }
    }
}

void Table(const std::vector<float>& a, int32_t w, int32_t h, std::vector<double>& s) {
    s.assign(static_cast<size_t>(w + 1) * (h + 1), 0.0);

    for (int32_t y = 0; y < h; y++) {
        double row = 0.0;

        for (int32_t x = 0; x < w; x++) {
            row += a[y * w + x];
            s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + row;
        }
    }
}

// local standard deviation and mean of y over radius r
void Contrast(const std::vector<float>& y, const std::vector<float>& y2, int32_t w, int32_t h, int32_t r, std::vector<float>& sd, std::vector<float>& m) {
    std::vector<double> s;
    std::vector<float> m2;
    Table(y, w, h, s);
    Box(s, w, h, r, m);
    Table(y2, w, h, s);
    Box(s, w, h, r, m2);
    sd.resize(m.size());

    for (size_t i = 0; i < m.size(); i++) {
        sd[i] = std::sqrt(std::max(m2[i] - m[i] * m[i], 0.0f));
    }
}

// mask id -> texels (ARGB8888); the texture callback reads them, an unknown id reads zero (no metal)
struct MaskData {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint32_t> texels;
};

std::unordered_map<uint32_t, MaskData> s_masks;
uint32_t s_nextId = 1;
std::vector<uint32_t> s_zero;

void MaskCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    if (command != GxTex_Latch) {
        return;
    }

    auto it = s_masks.find(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userArg)));
    texelStrideInBytes = 4 * width;

    if (it != s_masks.end() && it->second.w == width && it->second.h == height) {
        texels = it->second.texels.data();
    } else {
        s_zero.resize(static_cast<size_t>(width) * height, 0xFF0000FFu);
        texels = s_zero.data();
    }
}

}   // namespace

void ArmorMaskClassify(const uint8_t* bgra, int32_t w, int32_t h, float* metal, float* rough) {
    size_t n = static_cast<size_t>(w) * h;
    std::vector<float> y(n), y2(n), sd1, m1, sd2, m2;

    for (size_t i = 0; i < n; i++) {
        float r = bgra[4 * i + 2] / 255.0f, g = bgra[4 * i + 1] / 255.0f, b = bgra[4 * i] / 255.0f;
        y[i] = 0.299f * r + 0.587f * g + 0.114f * b;
        y2[i] = y[i] * y[i];
    }

    Contrast(y, y2, w, h, 1, sd1, m1);
    Contrast(y, y2, w, h, 6, sd2, m2);

    for (size_t i = 0; i < n; i++) {
        uint8_t rb = bgra[4 * i + 2], gb = bgra[4 * i + 1], bb = bgra[4 * i];
        float r = rb / 255.0f, g = gb / 255.0f, b = bb / 255.0f;
        float v = std::max(r, std::max(g, b));
        float mn = std::min(r, std::min(g, b));
        float con = std::max(sd1[i] * 2.5f, sd2[i] * 1.5f);           // the value range near and around the texel
        float flat = 1.0f - Ss(0.06f, 0.16f, con);                     // soft, low contrast: cloth, leather, fur
        float leather = (1.0f - Ss(0.14f, 0.30f, v)) * (1.0f - Ss(0.08f, 0.20f, con));   // dark and matte
        float sat = (v - mn) / std::max(v, 1e-3f);
        float warm = rb >= gb && gb >= bb ? 1.0f : 0.0f;
        float hi = std::max(y[i] - m2[i], 0.0f);                       // brighter than its surroundings
        float brown = warm * Ss(0.2f, 0.35f, sat) * (1.0f - Ss(0.38f, 0.55f, v)) * (1.0f - Ss(0.06f, 0.14f, hi));
        float nonmetal = std::max(std::max(leather, flat), brown) * (1.0f - Ss(0.5f, 0.65f, y[i]));   // bright stays metal
        metal[i] = (1.0f - nonmetal) * Ss(0.04f, 0.10f, v);           // black gaps are no metal either
        rough[i] = std::clamp(0.6f - 1.2f * hi, 0.25f, 0.7f);          // a highlight: polished
    }
}

bool ArmorMaskDecode(const void* blp, uint32_t w, uint32_t h, uint8_t* bgra) {
    CBLPFile file;
    uint8_t* data = nullptr;
    uint32_t stride = 0;

    if (!blp || !w || !h || !file.Source(const_cast<void*>(blp)) || !file.Lock(PIXEL_ARGB8888, 0, data, stride) || !data) {
        file.Close();
        return false;
    }

    uint32_t sw = file.Width(0), sh = file.Height(0);

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = data + stride * ((y * sh) / h);

        for (uint32_t x = 0; x < w; x++) {
            std::memcpy(bgra + 4 * (y * w + x), row + 4 * ((x * sw) / w), 4);
        }
    }

    // opaque files (alpha depth 0) decode with whatever is in the alpha byte
    if (file.m_header.m_alphaSize == 0) {
        for (uint32_t i = 0; i < w * h; i++) {
            bgra[4 * i + 3] = 255;
        }
    }

    file.Unlock(0);
    file.Close();
    return true;
}

uint32_t ArmorMaskUpload(uint32_t id, uint32_t w, uint32_t h, const uint32_t* texels, CTexture** texture) {
    bool create = !id || !*texture;
    id = id ? id : s_nextId++;
    MaskData& m = s_masks[id];
    m.w = w;
    m.h = h;
    m.texels.assign(texels, texels + static_cast<size_t>(w) * h);

    if (create) {
        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1);
        *texture = TextureCreate(w, h, GxTex_Argb8888, GxTex_Argb8888, flags, reinterpret_cast<void*>(static_cast<uintptr_t>(id)), MaskCallback, "ArmorMask", 0);
    } else if (CGxTex* gx = TextureGetGxTex(*texture, 1, nullptr)) {
        GxTexUpdate(gx, 0, 0, w, h, 1);
    }

    return id;
}

void ArmorMaskRelease(uint32_t id, CTexture* texture) {
    s_masks.erase(id);

    if (texture) {
        HandleClose(reinterpret_cast<HOBJECT>(texture));
    }
}

CTexture* ArmorMaskForFile(const char* path, uint32_t kind) {
    // ponytail: one mask per distinct helm/shoulder texture file, never freed (a few hundred KB over a session)
    static std::unordered_map<std::string, CTexture*> s_cache;

    if (!path || !*path || !kind) {
        return nullptr;
    }

    std::string key = std::string(path) + (kind == 2 ? "|p" : "|m");
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    auto it = s_cache.find(key);

    if (it != s_cache.end()) {
        return it->second;
    }

    CTexture* result = nullptr;
    SFileImpl* file = nullptr;

    std::string withExt = std::string(path) + ".blp";   // item texture paths usually come without the extension

    if ((SFile::OpenEx(nullptr, path, 0, &file) || SFile::OpenEx(nullptr, withExt.c_str(), 0, &file)) && file) {
        std::vector<uint8_t> bytes(SFileGetFileSize(file, nullptr));
        uint32_t got = 0;
        SFileReadFile(file, bytes.data(), static_cast<uint32_t>(bytes.size()), &got, nullptr, 0);
        SFileCloseFile(file);
        const BLPHeader* header = bytes.size() >= sizeof(BLPHeader) ? reinterpret_cast<const BLPHeader*>(bytes.data()) : nullptr;

        if (header && got == bytes.size() && header->m_width && header->m_height) {
            // a helm's alpha is not coverage (the stock env / glow masks), so it is not used here
            uint32_t w = std::min<uint32_t>(header->m_width, 256), h = std::max<uint32_t>(1, header->m_height * w / header->m_width);
            std::vector<uint8_t> bgra(4 * w * h);

            if (ArmorMaskDecode(bytes.data(), w, h, bgra.data())) {
                std::vector<float> metal(w * h), rough(w * h);
                std::vector<uint32_t> texels(w * h);
                ArmorMaskClassify(bgra.data(), w, h, metal.data(), rough.data());

                for (uint32_t i = 0; i < w * h; i++) {
                    uint32_t m = static_cast<uint32_t>(metal[i] * 255.0f + 0.5f), r = static_cast<uint32_t>(rough[i] * 255.0f + 0.5f);
                    texels[i] = 0xFF000000u | ((kind == 2 ? m : 0u) << 16) | ((kind == 1 ? m : 0u) << 8) | r;
                }

                ArmorMaskUpload(0, w, h, texels.data(), &result);
            }
        }
    }

    s_cache[key] = result;
    return result;
}

void ArmorMaskSetModel(CM2Model* model, CTexture* mask) {
    if (!model || model->m_pbrMask == mask) {
        return;
    }

    if (mask) {
        HandleDuplicate(reinterpret_cast<HOBJECT>(mask));
    }

    if (model->m_pbrMask) {
        HandleClose(reinterpret_cast<HOBJECT>(model->m_pbrMask));
    }

    model->m_pbrMask = mask;
}
