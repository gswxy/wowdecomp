#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_PACKAGES_MOPAQ_SCOMP_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_PACKAGES_MOPAQ_SCOMP_H

#include <atomic>
#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/Types.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

bool LzmaMemDecode(const uint8_t* source, uint32_t sourceSize, uint8_t* dest, uint32_t destSize, uint32_t* destProcessed, void* (*alloc)(uint32_t size), void (*free)(void* address));

namespace System_SComp {

typedef void (*Compressor)(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize, uint32_t* hint, uint8_t level);
typedef int32_t (*Decompressor)(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize);

struct COMPRESSOR {
    /* 0000 */ uint32_t m_type;
    /* 0004 */ Compressor m_func;
};

class BufferPool {
    public:
    /* 0000 */ uint8_t* m_base;
    /* 0004 */ uint32_t m_size;
    /* 0008 */ uint32_t m_count;
    /* 000C */ uint32_t m_used;

    uint8_t* Alloc();
};

struct PkwareBuffer {
    /* 0000 */ uint8_t* m_output;
    /* 0004 */ uint32_t m_outputPos;
    /* 0008 */ uint32_t m_outputSize;
    /* 000C */ const uint8_t* m_input;
    /* 0010 */ uint32_t m_inputSize;
};

struct ZlibAllocBuffer {
    /* 00000 */ uint8_t m_buffer[300000];
    /* 493E0 */ uint8_t* m_next;
};

extern Blizzard::Thread::LocalObject<ZlibAllocBuffer> s_zlibAllocBuffer;

// Not stock: 1 = inflate zlib sectors with libdeflate first (zlib retries on any libdeflate error), 0 = zlib only.
// Set by the mpqFastInflate CVar (Client_CVar.cpp).
extern std::atomic<bool> s_fastInflate;

void* alloc_wrap(uint32_t size);
void Bzip2Compress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize, uint32_t* hint, uint8_t level);
bool Compress(void* dst, uint32_t* dstSize, bool* checksum, const void* src, uint32_t srcSize, uint32_t count, const Blizzard::Mopaq::CompressionAlgorithm* algorithms);
bool Decompress(void* dst, uint32_t* dstSize, const void* src, uint32_t srcSize);
bool __stdcall DecompressBlock(void* dst, uint32_t* dstSize, const void* src, uint32_t srcSize, Decompressor* decompressors);
void free_wrap(void* address);
int32_t LzmaDecompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize);
void PkwareCompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize, uint32_t* hint, uint8_t level);
unsigned int PkwareBufferRead(void* param, unsigned char** buffer);
int PkwareBufferWrite(void* param, unsigned char* data, unsigned int size);
int32_t PkwareDecompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize);
void RLEZeroCompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize, uint32_t* hint, uint8_t level);
int32_t RLEZeroDecompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize);
void ZlibCompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize, uint32_t* hint, uint8_t level);
void* ZlibAlloc(void* opaque, unsigned int items, unsigned int size);
int32_t ZlibDecompress(void* dest, uint32_t* destSize, const void* source, uint32_t sourceSize);
void ZlibFree(void* opaque, void* address);
int zlib_compress(uint8_t* dest, uint32_t* destLen, const uint8_t* source, uint32_t sourceLen, int level);
int zlib_decompress(uint8_t* dest, uint32_t* destLen, const uint8_t* source, uint32_t sourceLen);

}

#endif
