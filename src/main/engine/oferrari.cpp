/***************************************************************************
    Ferrari palette extensions for CannonBall-SE.

    The original Ferrari implementation is kept verbatim in oferrari_base.cpp.
    This wrapper only extends its palette list and wraps tick() so F10 can
    cycle the selected colour immediately while the game is running.
***************************************************************************/

#include <SDL.h>

// Pre-include the original implementation's dependencies so the temporary
// macros below only affect tokens in oferrari_base.cpp itself.
#include "engine/oanimseq.hpp"
#include "engine/oattractai.hpp"
#include "engine/obonus.hpp"
#include "engine/ocrash.hpp"
#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/olevelobjs.hpp"
#include "engine/ooutputs.hpp"
#include "engine/ostats.hpp"
#include "engine/outils.hpp"
#include "engine/oferrari.hpp"

// Extend the existing five-colour initializer without modifying the preserved
// base implementation. PAL_CYAN occurs there only in FERRARI_PALETTES[].
#define PAL_CYAN PAL_CYAN, OFerrari::PAL_BLACK, OFerrari::PAL_WHITE, OFerrari::PAL_SILVER

// Keep the original tick logic as tick_base(); the wrapper below adds only the
// live colour hotkey and then delegates to the unchanged implementation.
#define tick tick_base
#include "oferrari_base.cpp"
#undef tick
#undef PAL_CYAN

void OFerrari::cycle_car_palette()
{
    const int size = sizeof(FERRARI_PALETTES) / sizeof(FERRARI_PALETTES[0]);

    config.engine.car_pal++;
    if (config.engine.car_pal >= size)
        config.engine.car_pal = 0;

    ferrari_pal = FERRARI_PALETTES[config.engine.car_pal];
}

void OFerrari::tick()
{
    static bool f10_was_down = false;

    const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
    const bool f10_down =
        keyboard_state && keyboard_state[SDL_SCANCODE_F10] != 0;

    if (f10_down && !f10_was_down)
        cycle_car_palette();

    f10_was_down = f10_down;
    tick_base();
}
