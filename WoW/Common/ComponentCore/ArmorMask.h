#ifndef WOW_COMMON_COMPONENTCORE_ARMORMASK_H
#define WOW_COMMON_COMPONENTCORE_ARMORMASK_H

#include <cstdint>

class CTexture;
class CM2Model;

// CUSTOM (ARMORMASK): the per-texel metal mask of worn mail and plate for PBR-lite (GxLighting.h
// GxLightingMaterial_Masked, stage kArmorMaskStage). Texel (ARGB8888): R plate metal, G mail metal, B roughness hint,
// A 255. The composite body's is built in CharacterComponent.cpp (PbrArmorMask) from the item layers that cover each
// texel; a helm's or shoulder's from its own texture (ArmorMaskForFile, once per file).

// The classifier, per texel of one mail/plate texture (bgra: w x h, B G R A bytes): metal and rough, 0..1. Covered
// texels are metal by default; leather (dark, matte or brown and unlit), cloth, fur and skin (soft, low local
// contrast) are taken out by local structure, not hue, so painted and enamelled metal stays metal. The reference and
// tuning harness is Tools/ArmorMask/classify.py; keep both in step.
void ArmorMaskClassify(const uint8_t* bgra, int32_t w, int32_t h, float* metal, float* rough);

// A BLP in memory (a whole file: header + mips) as w x h BGRA, nearest from mip 0; false when it cannot be decoded.
bool ArmorMaskDecode(const void* blp, uint32_t w, uint32_t h, uint8_t* bgra);

// The mask texture `id` (0: a new one) holding texels (w x h ARGB8888, copied): created on first use, else updated.
// Returns the id; *texture receives the texture (one handle owned by the mask; never closed for attachments).
uint32_t ArmorMaskUpload(uint32_t id, uint32_t w, uint32_t h, const uint32_t* texels, CTexture** texture);

// Forget mask `id`: its texture reads as no metal from then on (the device may still hold it). Closes the handle.
void ArmorMaskRelease(uint32_t id, CTexture* texture);

// An attached item's mask from its texture file (kind 1 mail, 2 plate), cached per file and kind; null when the file
// cannot be read.
CTexture* ArmorMaskForFile(const char* path, uint32_t kind);

// The model's mask (CM2Model::m_pbrMask), its own handle.
void ArmorMaskSetModel(CM2Model* model, CTexture* mask);

#endif
