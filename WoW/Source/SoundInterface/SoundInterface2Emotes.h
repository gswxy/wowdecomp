#ifndef WOW_SOURCE_SOUNDINTERFACE_SOUNDINTERFACE2EMOTES_H
#define WOW_SOURCE_SOUNDINTERFACE_SOUNDINTERFACE2EMOTES_H

#include <cstddef>
#include <cstdint>
#include "WoW/Common/Types.h"

struct TEXTEMOTELOOKUP {
    /* 0000 */ int32_t m_soundIds[NUM_RACES + 1][2];
    /* 00B0 */ int32_t m_emotesTextId;

    TEXTEMOTELOOKUP();
};

#endif
