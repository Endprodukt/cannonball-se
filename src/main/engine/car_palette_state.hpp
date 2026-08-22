#pragma once

#include <cstdint>

namespace car_palette_state
{
    const int COLOR_COUNT = 8;

    inline int default_color = 0;
    inline bool initialized = false;

    inline int normalize(int color)
    {
        return color >= 0 && color < COLOR_COUNT ? color : 0;
    }

    inline void initialize(int color)
    {
        default_color = normalize(color);
        initialized = true;
    }

    inline int get_default(int fallback)
    {
        return initialized ? default_color : normalize(fallback);
    }

    inline void set_default(int color)
    {
        default_color = normalize(color);
        initialized = true;
    }

    inline uint16_t palette_source(int color)
    {
        static const uint16_t PALETTES[COLOR_COUNT] =
        {
            2,   // Red
            256, // Blue
            261, // Yellow
            266, // Green
            271, // Cyan
            276, // Black
            281, // White
            286, // Silver
        };

        return PALETTES[normalize(color)];
    }
}
