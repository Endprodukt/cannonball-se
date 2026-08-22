/***************************************************************************
    Process Outputs - CannonBall-SE external output extensions.

    The existing output implementation, including SmartyPi console output,
    is retained verbatim in ooutputs_base.cpp. This wrapper adds MAME-
    compatible network and Windows outputs in parallel.
***************************************************************************/

#include "engine/ooutputs.hpp"
#include "engine/external_outputs.hpp"
#include "engine/external_output_settings.hpp"

// Keep the existing SmartyPi output implementation unchanged, but retain it
// under a private name so the public method can add the external transports.
#define writeDigitalToConsole writeDigitalToConsole_base
#include "engine/ooutputs_base.cpp"
#undef writeDigitalToConsole

#include "main.hpp"
#include "engine/oferrari.hpp"
#include "engine/ohud.hpp"
#include "engine/omusic.hpp"
#include "engine/oroad.hpp"
#include "engine/osprites.hpp"
#include "engine/ostats.hpp"
#include "engine/otraffic.hpp"
#include "sdl2/input.hpp"

namespace
{
    ExternalOutputs external_outputs;

    bool direct_view1_old = false;
    bool direct_view2_old = false;
    bool direct_view3_old = false;
    bool viewpoint_old = false;
    int music_mode_synced = -1;
    bool music_color_initialized = false;

    bool handle_direct_view_buttons(bool controls_active, bool attract_cycle_active)
    {
        const bool view1 = input.is_pressed(Input::VIEW1);
        const bool view2 = input.is_pressed(Input::VIEW2);
        const bool view3 = input.is_pressed(Input::VIEW3);
        const bool viewpoint = input.is_pressed(Input::VIEWPOINT);
        bool view_changed = false;

        if (controls_active)
        {
            if (view1 && !direct_view1_old)
            {
                oroad.set_view_mode(ORoad::VIEW_ORIGINAL);
                view_changed = true;
            }
            else if (view2 && !direct_view2_old)
            {
                oroad.set_view_mode(ORoad::VIEW_ELEVATED);
                view_changed = true;
            }
            else if (view3 && !direct_view3_old)
            {
                oroad.set_view_mode(ORoad::VIEW_INCAR);
                view_changed = true;
            }
            else if (attract_cycle_active && viewpoint && !viewpoint_old)
            {
                int mode = oroad.get_view_mode() + 1;
                if (mode > ORoad::VIEW_INCAR)
                    mode = ORoad::VIEW_ORIGINAL;

                oroad.set_view_mode(mode);
                view_changed = true;
            }
        }

        direct_view1_old = view1;
        direct_view2_old = view2;
        direct_view3_old = view3;
        viewpoint_old = viewpoint;
        return view_changed;
    }

    void sync_music_selection_mode(bool music_selection)
    {
        if (!music_selection)
        {
            music_mode_synced = -1;
            return;
        }

        const int selected = omusic.get_game_mode();

        // A newly selected Time Trial still needs the course map. Keep the
        // music-selection timer alive so the normal timeout cannot start a
        // stale/unselected Time Trial track. A legacy Time Trial has already
        // chosen its course, so its normal timer behaviour is preserved.
        if (selected == Outrun::MODE_TTRIAL &&
            outrun.cannonball_mode != Outrun::MODE_TTRIAL)
        {
            ostats.time_counter = config.sound.music_timer;
            ostats.frame_counter = ostats.frame_reset;
            music_mode_synced = selected;
            return;
        }

        if (selected != Outrun::MODE_ORIGINAL &&
            selected != Outrun::MODE_CONT)
        {
            music_mode_synced = selected;
            return;
        }

        // Keep the underlying engine mode aligned with what the player sees.
        // This also makes the original Music Select auto-timeout launch the
        // selected Original/Continuous mode rather than whichever mode booted.
        outrun.cannonball_mode = selected;

        if (selected != music_mode_synced)
        {
            config.load_scores(selected == Outrun::MODE_ORIGINAL);
            music_mode_synced = selected;
        }
    }

    void sync_continuous_traffic_to_difficulty()
    {
        if (outrun.cannonball_mode != Outrun::MODE_CONT)
            return;

        // Use the exact same stage/difficulty table as original OutRun. The
        // Continuous route visits all 15 stages, but stage_lookup_off / 8 still
        // identifies the original stage group (1..5) for traffic purposes.
        static const uint8_t ORIGINAL_TRAFFIC[] =
        {
            2, 2, 3, 4, 5, // Easy
            3, 4, 5, 6, 7, // Normal
            4, 5, 6, 7, 8, // Hard
            5, 6, 7, 8, 8, // Hardest
        };

        int difficulty = config.engine.dip_traffic;
        if (difficulty < 0)
            difficulty = 0;
        else if (difficulty > 3)
            difficulty = 3;

        int stage_group = oroad.stage_lookup_off / 8;
        if (stage_group < 0)
            stage_group = 0;
        else if (stage_group > 4)
            stage_group = 4;

        const uint8_t traffic =
            ORIGINAL_TRAFFIC[(difficulty * 5) + stage_group];

        // Keep both values synchronized. OTraffic::set_max_traffic() samples
        // custom_traffic at checkpoints; updating max_traffic too ensures the
        // new stage-group value takes effect immediately on that same frame.
        outrun.custom_traffic = traffic;
        otraffic.set_custom_max_traffic(traffic);
    }

    void sync_music_car_color(bool music_selection)
    {
        if (!music_selection)
        {
            music_color_initialized = false;
            return;
        }

        if (!music_color_initialized)
        {
            // Start from the persisted Ferrari colour. F10 in attract mode
            // updates this default, while the shifter can still make a
            // temporary per-run choice on the Music Select screen.
            music_color_initialized = true;
        }
    }

    void draw_music_color_preview(bool music_selection)
    {
        if (!music_selection || outrun.game_state != GS_MUSIC)
            return;

        // OMusic used an experimental centre-screen COLOR row while this
        // feature was being prototyped. Clear both affected rows so the final
        // presentation leaves the centre of the original Music Select clean.
        for (int x = 0; x < 40; x++)
        {
            video.write_text16(ohud.translate(x, 14), 0);
            video.write_text16(ohud.translate(x, 15), 0);
        }

        // Reuse the exact single-row font and palette of FREE PLAY. The lower
        // line shares the FREE PLAY row, while LOW - HI sits one row above it.
        // Both lines use the same centre point so the block stays symmetrical.
        uint32_t freeplay_record = TEXT1_FREEPLAY;
        const uint32_t freeplay_dst = roms.rom0.read32(&freeplay_record);
        roms.rom0.read16(&freeplay_record); // tile count
        const uint16_t freeplay_data = roms.rom0.read16(&freeplay_record);
        const uint16_t freeplay_pal = (freeplay_data >> 8) & 0xFF;

        const uint32_t freeplay_relative =
            (freeplay_dst - 0x110030) & 0x0FFF;
        const uint16_t freeplay_y =
            static_cast<uint16_t>(freeplay_relative / 0x80);

        const char* line1 = "LOW - HI";
        const char* line2 = "CHANGE COLOR";
        const int line1_length = 8;
        const int line2_length = 12;
        const int line2_x = 40 - line2_length;
        const int line1_x = line2_x + ((line2_length - line1_length) / 2);
        const uint16_t line1_y = freeplay_y > 0 ? freeplay_y - 1 : freeplay_y;

        // Clear only the compact right-hand block. FREE PLAY on the left remains
        // untouched and is still drawn by the original HUD code.
        for (int x = line2_x; x < 40; x++)
        {
            video.write_text16(ohud.translate(x, line1_y), 0);
            video.write_text16(ohud.translate(x, freeplay_y), 0);
        }

        ohud.blit_text_new(
            static_cast<uint16_t>(line1_x),
            line1_y,
            line1,
            freeplay_pal);

        ohud.blit_text_new(
            static_cast<uint16_t>(line2_x),
            freeplay_y,
            line2,
            freeplay_pal);

        // Reuse the small Ferrari that normally drives across the course map.
        // The map entry supplies its native size/anchor properties, while the
        // palette follows the same eight Ferrari colours as the full-size car.
        const int preview_index = (OSprites::SPRITE_ENTRIES - 0x10) + 5;
        oentry* preview = &osprites.jump_table[preview_index];
        preview->init(preview_index);

        const uint32_t map_ferrari_entry =
            outrun.adr.sprite_coursemap + (25 * 20);

        preview->draw_props = roms.rom0p->read8(map_ferrari_entry + 1);
        preview->shadow = roms.rom0p->read8(map_ferrari_entry + 2);
        preview->zoom = roms.rom0p->read8(map_ferrari_entry + 3);

        // Centre the preview directly above the '-' in LOW - HI. Sprite X is
        // relative to the 320-pixel playfield centre, while text X is in tiles.
        const int dash_x = line1_x + 4;
        preview->x = static_cast<int16_t>(((dash_x * 8) + 4) - 160);
        preview->y = static_cast<int16_t>((line1_y * 8) - 8);
        preview->priority = 0x1FF;
        preview->road_priority = 0x1FF;
        preview->addr = outrun.adr.sprite_minicar_right;

        static const uint16_t CAR_PALETTES[] =
        {
            OFerrari::PAL_RED,
            OFerrari::PAL_BLUE,
            OFerrari::PAL_YELLOW,
            OFerrari::PAL_GREEN,
            OFerrari::PAL_CYAN,
            OFerrari::PAL_BLACK,
            OFerrari::PAL_WHITE,
            OFerrari::PAL_SILVER,
        };

        int color = config.engine.car_pal;
        if (color < 0 || color >= 8)
            color = 0;

        preview->pal_src = CAR_PALETTES[color];
        osprites.map_palette(preview);

        // writeDigitalToConsole() runs after the current engine frame has been
        // assembled. Queue the preview here and it joins the Music Select sprite
        // list on the following frame; doing this every frame keeps it stable.
        osprites.do_spr_order_shadows(preview);
    }
}

void OOutputs::writeDigitalToConsole()
{
    const bool music_selection =
        cannonball::state == cannonball::STATE_GAME &&
        (outrun.game_state == GS_INIT_MUSIC ||
         outrun.game_state == GS_MUSIC);

    // Keep the visible Music Select choice and the underlying mode in sync,
    // including the original automatic Music Select timeout behaviour.
    sync_music_selection_mode(music_selection);

    // Keep Continuous traffic tied to the normal OutRun difficulty setting.
    // This is intentionally independent of whether external outputs are enabled;
    // this method is already called every engine tick by main.cpp.
    sync_continuous_traffic_to_difficulty();

    // Keep the persisted default Ferrari colour when Music Select opens.
    // The player can still override it for the next run with the shifter.
    sync_music_car_color(music_selection);

    // Draw the car-colour instruction and queue the small map Ferrari preview.
    draw_music_color_preview(music_selection);

    // Preserve the original SmartyPi console output path exactly as before.
    writeDigitalToConsole_base();

    const bool enhanced_attract_driving =
        cannonball::state == cannonball::STATE_GAME &&
        config.engine.new_attract &&
        outrun.game_state == GS_ATTRACT;

    const bool race_view_controls_active =
        outrun.game_state >= GS_START1 &&
        outrun.game_state <= GS_INGAME;

    // Player view controls also work while the enhanced attract driving scene
    // is active. This does not touch the attract timer or attract_view sequence,
    // so the existing automatic view changes continue on their normal cadence.
    const bool attract_manual_view_changed =
        handle_direct_view_buttons(
            race_view_controls_active || enhanced_attract_driving,
            enhanced_attract_driving) &&
        enhanced_attract_driving;

    // During the race the single VIEW lamp remains the normal availability
    // lamp. During music selection it becomes a mode-selection lamp. During
    // enhanced attract driving the dedicated VIEW1/2/3 lamps provide the show,
    // so the legacy single VIEW lamp stays off there.
    const bool view_lamp_active =
        outrun.game_state >= GS_START1 &&
        outrun.game_state < GS_INIT_GAMEOVER;

    const uint8_t view = oroad.get_view_mode();

    // Mirror only the existing enhanced-attract automatic timer. This lets the
    // lamp logic distinguish an automatic view from a manual override without
    // changing the actual automatic sequence in Outrun::tick_attract().
    static bool attract_sequence_tracking = false;
    static int attract_sequence_counter = 0;
    static uint8_t attract_sequence_view = 0;
    static bool attract_current_view_automatic = true;
    static bool attract_original_intro = false;
    static uint32_t attract_original_intro_start = 0;
    static uint32_t attract_original_chase_start = 0;

    if (enhanced_attract_driving)
    {
        bool automatic_view_changed = false;

        if (!attract_sequence_tracking)
        {
            attract_sequence_tracking = true;
            attract_sequence_counter = outrun.tick_frame ? 1 : 0;
            attract_sequence_view = 0;
            attract_current_view_automatic = true;
            attract_original_intro = false;
            attract_original_intro_start = 0;
            attract_original_chase_start = outrun.tick_counter;
        }
        else if (outrun.tick_frame && ++attract_sequence_counter > 240)
        {
            attract_sequence_counter = 0;
            if (++attract_sequence_view > 2)
                attract_sequence_view = 0;
            automatic_view_changed = true;
        }

        if (automatic_view_changed)
        {
            attract_current_view_automatic = true;

            if (attract_sequence_view == 0)
            {
                // Only an automatic return to ORIGINAL gets the three-flash
                // intro and subsequent ping-pong chase.
                attract_original_intro = true;
                attract_original_intro_start = outrun.tick_counter;
                attract_original_chase_start = outrun.tick_counter + 48;
            }
            else
            {
                attract_original_intro = false;
                attract_original_intro_start = 0;
                attract_original_chase_start = 0;
            }
        }

        // Manual input happens after the engine's automatic attract update in
        // the current frame, so a manual choice deliberately wins if both occur.
        if (attract_manual_view_changed)
        {
            attract_current_view_automatic = false;
            attract_original_intro = false;
            attract_original_intro_start = 0;
            attract_original_chase_start = 0;
        }
    }
    else
    {
        attract_sequence_tracking = false;
        attract_sequence_counter = 0;
        attract_sequence_view = 0;
        attract_current_view_automatic = true;
        attract_original_intro = false;
        attract_original_intro_start = 0;
        attract_original_chase_start = 0;
    }

    uint32_t attract_original_intro_elapsed = 0;
    if (attract_original_intro)
    {
        attract_original_intro_elapsed =
            outrun.tick_counter - attract_original_intro_start;

        // All view-lamp effects run twice as fast as START: 8 ticks on,
        // 8 ticks off. Three complete flashes therefore take 48 ticks.
        if (attract_original_intro_elapsed >= 48)
            attract_original_intro = false;
    }

    // MAMEHooker START lamp behaviour is deliberately cabinet-oriented rather
    // than tied to the original D_START_LAMP bit. In particular, CannonBall's
    // freeplay PRESS START text does not set the original hardware bit.
    const bool press_start_screen =
        outrun.game_state == GS_ATTRACT ||
        outrun.game_state == GS_BEST1 ||
        outrun.game_state == GS_LOGO ||
        outrun.game_state == GS_BEST2;

    const bool press_start_available =
        config.engine.freeplay || ostats.credits > 0;

    // START keeps its original BIT_4 blink cadence.
    const bool start_lamp_blink =
        ((press_start_screen && press_start_available) ||
         music_selection) &&
        (outrun.tick_counter & BIT_4);

    // As soon as music selection hands off to the game, keep START steadily
    // illuminated through the driving sequence, race and bonus sequence.
    const bool start_lamp_ingame =
        outrun.game_state >= GS_INIT_GAME &&
        outrun.game_state <= GS_BONUS;

    // Music-select game mode indication remains unchanged.
    const bool mode_lamp_blink =
        music_selection &&
        (outrun.tick_counter & BIT_4);

    // Every Attract-mode view-button blink runs at BIT_3, twice the frequency
    // of the START lamp's BIT_4 cadence.
    const bool attract_view_blink_fast =
        enhanced_attract_driving &&
        (outrun.tick_counter & BIT_3);

    const bool attract_original_intro_lit =
        attract_original_intro &&
        (((attract_original_intro_elapsed >> 3) & 1) == 0);

    // Automatic ORIGINAL: after the three-flash intro, run the ping-pong chase
    // VIEW1 -> VIEW2 -> VIEW3 -> VIEW2 -> ... at the same fast step rate.
    uint8_t attract_chase_phase = 0;
    if (attract_current_view_automatic &&
        view == ORoad::VIEW_ORIGINAL &&
        !attract_original_intro)
    {
        const uint32_t chase_elapsed =
            outrun.tick_counter - attract_original_chase_start;
        attract_chase_phase =
            static_cast<uint8_t>((chase_elapsed >> 3) & 3);
    }

    const bool attract_chase_active =
        enhanced_attract_driving &&
        attract_current_view_automatic &&
        view == ORoad::VIEW_ORIGINAL &&
        !attract_original_intro;

    const bool attract_chase_view1 =
        attract_chase_active &&
        attract_chase_phase == 0;

    const bool attract_chase_view2 =
        attract_chase_active &&
        (attract_chase_phase == 1 || attract_chase_phase == 3);

    const bool attract_chase_view3 =
        attract_chase_active &&
        attract_chase_phase == 2;

    // Manual ORIGINAL is deliberately different from automatic ORIGINAL:
    // only VIEW1 blinks, just like VIEW2/VIEW3 when selected manually.
    const bool attract_manual_view1_blink =
        enhanced_attract_driving &&
        !attract_current_view_automatic &&
        view == ORoad::VIEW_ORIGINAL &&
        attract_view_blink_fast;

    const int selected_game_mode = omusic.get_game_mode();

    const auto& settings = external_output_settings();

    external_outputs.update(
        settings.network,
        settings.windows,
        settings.port,
        cannonball::state != cannonball::STATE_QUIT,
        (start_lamp_blink || start_lamp_ingame) ? 1 : 0,
        is_set(D_BRAKE_LAMP),
        music_selection
            ? (mode_lamp_blink ? 1 : 0)
            : (enhanced_attract_driving
                ? 0
                : (view_lamp_active ? 1 : 0)),
        music_selection
            ? ((mode_lamp_blink && selected_game_mode == Outrun::MODE_ORIGINAL) ? 1 : 0)
            : (enhanced_attract_driving
                ? ((view == ORoad::VIEW_ORIGINAL)
                    ? ((attract_original_intro_lit ||
                        attract_chase_view1 ||
                        attract_manual_view1_blink) ? 1 : 0)
                    : 0)
                : ((view_lamp_active && view == ORoad::VIEW_ORIGINAL) ? 1 : 0)),
        music_selection
            ? ((mode_lamp_blink && selected_game_mode == Outrun::MODE_CONT) ? 1 : 0)
            : (enhanced_attract_driving
                ? ((view == ORoad::VIEW_ORIGINAL)
                    ? ((attract_original_intro_lit || attract_chase_view2) ? 1 : 0)
                    : ((view == ORoad::VIEW_ELEVATED && attract_view_blink_fast) ? 1 : 0))
                : ((view_lamp_active && view == ORoad::VIEW_ELEVATED) ? 1 : 0)),
        music_selection
            ? ((mode_lamp_blink && selected_game_mode == Outrun::MODE_TTRIAL) ? 1 : 0)
            : (enhanced_attract_driving
                ? ((view == ORoad::VIEW_ORIGINAL)
                    ? ((attract_original_intro_lit || attract_chase_view3) ? 1 : 0)
                    : ((view == ORoad::VIEW_INCAR && attract_view_blink_fast) ? 1 : 0))
                : ((view_lamp_active && view == ORoad::VIEW_INCAR) ? 1 : 0)));
}
