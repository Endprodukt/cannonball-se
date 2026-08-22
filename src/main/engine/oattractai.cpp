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
#include "engine/car_palette_state.hpp"
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
    inline void restore_attract_default_palette()
    {
        const int color =
            car_palette_state::get_default(config.engine.car_pal);

        config.engine.car_pal = color;
        oferrari.ferrari_pal = car_palette_state::palette_source(color);
    }

    inline void handle_attract_palette_hotkey()
    {
        if (car_palette_hotkey::pressed())
        {
            oferrari.cycle_car_palette();
            car_palette_state::set_default(config.engine.car_pal);
            config.save();
        }
    }
}

void OAttractAI::tick_ai_enhanced()
{
    // Any temporary Music Select/race colour ends when attract driving resumes.
    restore_attract_default_palette();
    handle_attract_palette_hotkey();
    tick_ai_enhanced_base();
}

void OAttractAI::tick_ai()
{
    restore_attract_default_palette();
    handle_attract_palette_hotkey();
    tick_ai_base();
}
