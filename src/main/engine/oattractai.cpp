/***************************************************************************
    Attract-mode Ferrari palette hotkey wrapper.

    The original AI implementation is preserved in oattractai_palette_base.cpp.
    Both original and enhanced attract AI paths share the same F10 edge state as
    normal gameplay, so the Ferrari colour can be changed while the demo runs
    without touching any AI behaviour.
***************************************************************************/

#include <cstdlib>
#include <time.h>

#include "engine/car_palette_hotkey.hpp"
#include "engine/oattractai.hpp"
#include "engine/oferrari.hpp"
#include "engine/oinputs.hpp"
#include "engine/ostats.hpp"
#include "engine/otraffic.hpp"

#define tick_ai_enhanced tick_ai_enhanced_base
#define tick_ai tick_ai_base
#include "oattractai_palette_base.cpp"
#undef tick_ai
#undef tick_ai_enhanced

namespace
{
    inline void handle_attract_palette_hotkey()
    {
        if (car_palette_hotkey::pressed())
        {
            oferrari.cycle_car_palette();

            // F10 in attract mode changes the user's persistent default colour.
            // Save at the exact handler that receives the working attract input,
            // rather than relying on the normal Ferrari tick path.
            config.save();
        }
    }
}

void OAttractAI::tick_ai_enhanced()
{
    handle_attract_palette_hotkey();
    tick_ai_enhanced_base();
}

void OAttractAI::tick_ai()
{
    handle_attract_palette_hotkey();
    tick_ai_base();
}
