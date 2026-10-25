#ifndef ENGINE_SOURCE_FFX_EFFECTGLOW_H
#define ENGINE_SOURCE_FFX_EFFECTGLOW_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/FFX/FFX.h"

class EffectGlow : public FFX::Effect {
    public:
    enum PASS_SEQUENCE : int32_t {
        STANDARD_PASSES = 0,
        ALTERNATE_PASSES = 1,
    };

    /* 001C */ TSGrowableArray<FFX::Pass*> m_altPasses;
    /* 002C */ PASS_SEQUENCE m_mode;

    virtual ~EffectGlow();
    virtual bool Valid();
    virtual void Execute();
    virtual void SetParameters(int32_t count, const int32_t* params);

    EffectGlow();
};

#endif
