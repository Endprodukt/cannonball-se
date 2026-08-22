/***************************************************************************
    Music-screen Ferrari palette extension wrapper.

    The current music selector implementation is preserved verbatim in
    omusic_palette_base.cpp. This wrapper extends only its car-colour range
    from five to eight colours and repaints the colour indicator for the three
    grayscale additions, while leaving all existing mode/music/FFB behaviour
    unchanged.
***************************************************************************/

#include "main.hpp"
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
#define tick tick_base
#define check_start check_start_base
#include "omusic_palette_base.cpp"
#undef check_start
#undef tick
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

    void draw_extended_color_swatch()
    {
        // Same compact swatch used by the existing music screen, now with the
        // three grayscale colours represented by matching neutral shades.
        static const uint16_t CAR_COLORS[CAR_COLOR_COUNT] =
        {
            0x100F, // Red
            0x4F00, // Blue
            0x30FF, // Yellow
            0x20F0, // Green
            0x6FF0, // Cyan
            0x0444, // Black / anthracite highlight
            0x0FFF, // White
            0x0AAA, // Silver
        };

        const int color = wrap_car_color(config.engine.car_pal);

        uint32_t pal_addr = 0x120000 + (7 * 0x20);
        video.write_pal16(&pal_addr, 0);
        for (int i = 1; i < 16; i++)
            video.write_pal16(&pal_addr, CAR_COLORS[color]);

        video.write_text16(ohud.translate(22, 14), 0x8FFD);
    }
}

void OMusic::enable()
{
    enable_base();

    if (!skip_music_tick)
        draw_extended_color_swatch();
}

void OMusic::check_start()
{
    int old_color = config.engine.car_pal;
    old_color = wrap_car_color(old_color);

    int color_direction = 0;

    // Mirror only the direction detection from the preserved implementation.
    // check_start_base() still owns all actual input handling, game-mode
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
        // If a colour shift and START happened on the same frame, overwrite
        // that save with the corrected eight-colour value.
        if (save_after_correction)
            config.save();
    }

    if (outrun.game_state == GS_MUSIC)
        draw_extended_color_swatch();
}

void OMusic::tick()
{
    tick_base();

    // draw_game_options() in the preserved implementation still paints its
    // original five-colour swatch each frame. Repaint just that tiny indicator
    // afterwards so Black, White and Silver remain visible.
    if (outrun.game_state == GS_MUSIC)
        draw_extended_color_swatch();
}
