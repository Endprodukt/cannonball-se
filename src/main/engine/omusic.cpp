/***************************************************************************
    Music-screen Ferrari palette extension wrapper.

    The current music selector implementation is preserved in
    omusic_palette_base.cpp. This wrapper extends its shifter colour selection
    from five to eight Ferrari palettes while leaving all existing
    mode/music/FFB behaviour unchanged.
***************************************************************************/

#include "main.hpp"
#include "engine/car_palette_state.hpp"
#include "engine/oferrari.hpp"
#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/ologo.hpp"
#include "engine/omusic.hpp"
#include "engine/otiles.hpp"
#include "engine/otraffic.hpp"
#include "engine/ostats.hpp"
#include "frontend/menu.hpp"
#include "directx/ffeedback.hpp"

#define enable enable_base
#define check_start check_start_base
#include "omusic_palette_base.cpp"
#undef check_start
#undef enable

namespace
{
    const int CAR_COLOR_COUNT = 8;

    int wrap_car_color(int color)
    {
        while (color < 0)
            color += CAR_COLOR_COUNT;
        while (color >= CAR_COLOR_COUNT)
            color -= CAR_COLOR_COUNT;
        return color;
    }
}

void OMusic::enable()
{
    // A fresh Music Select always starts from the persistent attract/default
    // colour. The shifter may then choose a temporary colour for this race.
    // The second Time Trial handoff is the same race, so preserve its already
    // selected temporary colour when returning from course selection.
    if (!(return_from_time_trial &&
          outrun.cannonball_mode == Outrun::MODE_TTRIAL))
    {
        config.engine.car_pal =
            car_palette_state::get_default(config.engine.car_pal);
    }

    enable_base();
}

void OMusic::check_start()
{
    int old_color = wrap_car_color(config.engine.car_pal);
    int color_direction = 0;

    // Mirror only the direction detection from the preserved implementation.
    // check_start_base() still owns the actual input handling, game-mode
    // selection, FFB cleanup and start transitions.
    if (config.controls.gear == config.controls.GEAR_PRESS)
    {
        const bool low_now = input.is_pressed(Input::GEAR1);
        if (menu_gear_initialized && low_now != menu_gear_state)
            color_direction = low_now ? -1 : 1;
    }
    else if (config.controls.gear == config.controls.GEAR_SEPARATE)
    {
        if (input.has_pressed(Input::GEAR1))
            color_direction = -1;
        else if (input.has_pressed(Input::GEAR2))
            color_direction = 1;
    }
    else if (config.controls.gear == config.controls.GEAR_BUTTON)
    {
        if (input.has_pressed(Input::GEAR1))
            color_direction = oinputs.gear ? 1 : -1;
    }
    else
    {
        if (input.has_pressed(Input::GEAR1))
            color_direction = -1;
        else if (input.has_pressed(Input::GEAR2))
            color_direction = 1;
    }

    const bool save_after_correction =
        color_direction != 0 &&
        ostats.credits &&
        input.has_pressed(Input::START);

    check_start_base();

    if (color_direction != 0)
    {
        config.engine.car_pal =
            wrap_car_color(old_color + color_direction);

        // The preserved five-colour routine may already have saved on START.
        // Config::save() now always persists the separate attract/default
        // colour, so this correction remains race-only even on the START frame.
        if (save_after_correction)
            config.save();
    }
}
