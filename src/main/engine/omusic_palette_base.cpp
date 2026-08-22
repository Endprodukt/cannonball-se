/***************************************************************************
    Music Selection Screen.

    This is a combination of a tilemap and overlayed sprites.

    Copyright Chris White.
    See license.txt for more details.
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

extern Menu* menu;

OMusic omusic;

OMusic::OMusic(void)
{
    tilemap    = NULL;
    tile_patch = NULL;
    ffb_detent_spring_applied = -1;
    ffb_detent_target_applied = 0;
    game_mode_selected = Outrun::MODE_ORIGINAL;
    menu_gear_initialized = false;
    menu_gear_state = false;
    return_from_time_trial = false;
    skip_music_tick = false;
    pending_music_selected = 0;
}


OMusic::~OMusic(void)
{
    if (tilemap)    delete tilemap;
    if (tile_patch) delete tile_patch;
}

void OMusic::set_game_mode(int mode)
{
    if (mode == Outrun::MODE_ORIGINAL ||
        mode == Outrun::MODE_CONT ||
        mode == Outrun::MODE_TTRIAL)
    {
        game_mode_selected = mode;
    }
}

void OMusic::cycle_game_mode()
{
    if (game_mode_selected == Outrun::MODE_ORIGINAL)
        set_game_mode(Outrun::MODE_CONT);
    else if (game_mode_selected == Outrun::MODE_CONT)
        set_game_mode(Outrun::MODE_TTRIAL);
    else
        set_game_mode(Outrun::MODE_ORIGINAL);
}

void OMusic::cycle_car_color(int direction)
{
    int color = config.engine.car_pal + direction;

    if (color < 0)
        color = 4;
    else if (color > 4)
        color = 0;

    config.engine.car_pal = color;
}

void OMusic::set_continuous_traffic_from_difficulty()
{
    // Continuous no longer needs a separate traffic choice when launched from
    // the arcade flow. Start with the same traffic density as stage 1 of the
    // selected original-game difficulty. The existing continuous setting is
    // retained only for backwards compatibility with the legacy frontend.
    static const uint8_t TRAFFIC_BY_DIFFICULTY[] = { 2, 3, 4, 5 };

    int difficulty = config.engine.dip_traffic;
    if (difficulty < 0)
        difficulty = 0;
    else if (difficulty > 3)
        difficulty = 3;

    outrun.custom_traffic = TRAFFIC_BY_DIFFICULTY[difficulty];
}

void OMusic::draw_color_swatch(uint16_t x, uint16_t y)
{
    // Reserve text palette 7 for the music-screen colour swatch. Fill all of
    // its visible colour entries with one RGB value, then draw the wide rev-bar
    // tile as a compact solid block. The normal game palette is rebuilt during
    // GS_INIT_GAME, so this temporary palette cannot leak into the race HUD.
    static const uint16_t CAR_COLORS[] =
    {
        0x100F, // Red
        0x4F00, // Blue
        0x30FF, // Yellow
        0x20F0, // Green
        0x6FF0, // Cyan
    };

    int color = config.engine.car_pal;
    if (color < 0 || color > 4)
        color = 0;

    uint32_t pal_addr = 0x120000 + (7 * 0x20);
    video.write_pal16(&pal_addr, 0);
    for (int i = 1; i < 16; i++)
        video.write_pal16(&pal_addr, CAR_COLORS[color]);

    // Priority + palette 7 + tile 0x1FD (the filled/wide rev-counter segment).
    video.write_text16(ohud.translate(x, y), 0x8FFD);
}

void OMusic::draw_game_options()
{
    const char* mode_name = "ORIGINAL";

    if (game_mode_selected == Outrun::MODE_CONT)
        mode_name = "CONTINUOUS";
    else if (game_mode_selected == Outrun::MODE_TTRIAL)
        mode_name = "TIME TRIAL";

    // Draw arbitrary text with the same two-row System 16 font used by the
    // original SELECT MUSIC prompt. Supplying the palette base separately lets
    // the instruction use the exact ROM style while the selected mode uses the
    // same yellow/black style as the song title.
    auto draw_double_row_centered = [](uint8_t y, const char* text, uint16_t pal)
    {
        int length = static_cast<int>(strlen(text));
        if (length > 40)
            length = 40;

        const int x_start = 20 - (length / 2);

        // Clear both rows first, since the three mode names have different
        // lengths and otherwise leave characters behind when cycling.
        for (int x = 0; x < 40; x++)
        {
            video.write_text16(ohud.translate(x, y), 0);
            video.write_text16(ohud.translate(x, y) + 0x80, 0);
        }

        uint32_t dst_addr = ohud.translate(x_start, y);

        for (int i = 0; i < length; i++)
        {
            uint16_t c = static_cast<uint8_t>(text[i]);

            if (c >= 'a' && c <= 'z')
                c -= 0x20;

            if (c == ' ')
            {
                video.write_text16(&dst_addr, 0);
                video.write_text16(0x7E + dst_addr, 0);
            }
            else if (c >= 'A' && c <= 'Z')
            {
                c = ((c - 'A') * 2) + pal;
                video.write_text16(&dst_addr, c);
                video.write_text16(0x7E + dst_addr, c + 1);
            }
        }
    };

    // Extract the palette directly from the original SELECT MUSIC BY STEERING
    // ROM text record, so the new instruction has exactly the same colours.
    uint32_t select_music_style = TEXT2_SELECT_MUSIC + 2;
    uint16_t prompt_pal = roms.rom0.read8(&select_music_style);
    prompt_pal = 0x80A0 | ((prompt_pal << 9) | ((prompt_pal >> 7) & 1));

    draw_double_row_centered(
        3,
        "SELECT GAME MODE WITH VIEW BUTTONS",
        prompt_pal);

    // 0x8AA0 is the palette used by the enhanced song title when music notes
    // are enabled. Use the same font/palette here, but deliberately omit notes.
    draw_double_row_centered(5, mode_name, 0x8AA0);

    // Keep the car-colour control visible but unobtrusive below the song title.
    // This replaces the large block of helper text that previously covered the
    // radio artwork.
    ohud.blit_text_new(0, 14, "                                        ", OHud::GREY);
    ohud.blit_text_new(16, 14, "COLOR", OHud::GREY);
    draw_color_swatch(22, 14);
}

int OMusic::get_music_selected()
{
    apply_music_detent_ffb();

    // Keep the older event-based music detent in main.cpp quiet. The actual
    // selection remains in music_selected and is used directly by OMusic.
    return 0;
}

int OMusic::track_from_steering(int steering) const
{
    if (total_tracks <= 1)
        return 0;

    if (steering < -127)
        steering = -127;
    else if (steering > 127)
        steering = 127;

    // Divide the full -127..+127 steering range into one equal-width zone per
    // track. Using 255 as the divisor keeps +127 inside the final track zone.
    const int normalized = steering + 127;
    int track = (normalized * total_tracks) / 255;

    if (track < 0)
        track = 0;
    else if (track >= total_tracks)
        track = total_tracks - 1;

    return track;
}

int OMusic::steering_for_track(int track) const
{
    if (total_tracks <= 1)
        return 0;

    if (track < 0)
        track = 0;
    else if (track >= total_tracks)
        track = total_tracks - 1;

    // Return the centre of this track's steering zone. With three tracks this
    // naturally produces the familiar left / centre / right positions; extra
    // tracks are inserted evenly between them.
    const int normalized =
        ((2 * track + 1) * 255) /
        (2 * total_tracks);

    int steering = normalized - 127;

    if (steering < -127)
        steering = -127;
    else if (steering > 127)
        steering = 127;

    return steering;
}

void OMusic::apply_music_detent_ffb()
{
    if (!config.controls.haptic ||
        !forcefeedback::is_supported() ||
        total_tracks <= 0)
    {
        return;
    }

    // More tracks means closer virtual switch positions. Strengthen the menu
    // spring progressively so those positions remain mechanically distinct.
    // Three tracks retain roughly the current feel; the boost is capped so a
    // large custom playlist cannot make the selector excessively heavy.
    int spring_strength = config.controls.centering_strength;

    if (spring_strength < 30)
        spring_strength = 30;

    if (total_tracks > 3)
        spring_strength += (total_tracks - 3) * 5;

    if (spring_strength > 80)
        spring_strength = 80;

    if (spring_strength != ffb_detent_spring_applied)
    {
        forcefeedback::set_centering_strength(spring_strength);
        ffb_detent_spring_applied = spring_strength;
    }

    const int target = steering_for_track(cursor_pos);

    if (target == 0)
    {
        if (ffb_detent_target_applied != 0)
            forcefeedback::stop();

        ffb_detent_target_applied = 0;
        return;
    }

    const int target_abs = target < 0 ? -target : target;

    // Balance a continuous ConstantForce against the native centre spring.
    // Using the strongest raw ConstantForce level and scaling only its gain
    // keeps the equilibrium point close to the centre of each selected zone.
    int detent_gain =
        (spring_strength * target_abs + 63) /
        127;

    if (detent_gain < 10)
        detent_gain = 10;
    else if (detent_gain > 100)
        detent_gain = 100;

    forcefeedback::set_gain(detent_gain);
    forcefeedback::set(
        target < 0 ? 0x09 : 0x07,
        0);
    forcefeedback::set_gain(config.controls.ffb_strength);

    ffb_detent_target_applied = target;
}

void OMusic::reset_music_detent_ffb()
{
    if (forcefeedback::is_supported())
    {
        forcefeedback::stop();
        forcefeedback::set_gain(config.controls.ffb_strength);
        forcefeedback::set_centering_strength(
            config.controls.centering_strength);
    }

    ffb_detent_spring_applied = -1;
    ffb_detent_target_applied = 0;
}

// Load Modified Widescreen version of tilemap
bool OMusic::load_widescreen_map(std::string path)
{
    int status = 0;

    if (tilemap == NULL)
    {
        tilemap = new RomLoader();
        status += tilemap->load_binary(std::string(path + "tilemap.bin").c_str());
    }

    if (tile_patch == NULL)
    {
        tile_patch = new RomLoader();
        status += tile_patch->load_binary(std::string(path + "tilepatch.bin").c_str());
    }

    return status == 0;
}

// Initialize Music Selection Screen
//
// Source: 0xB342
void OMusic::enable()
{
    // Returning from the Time Trial course map normally causes the engine to
    // visit GS_INIT_MUSIC again. If this Time Trial was launched from the music
    // screen, preserve the song already chosen and turn that second visit into
    // a one-tick handoff straight to GS_INIT_GAME.
    if (return_from_time_trial && outrun.cannonball_mode == Outrun::MODE_TTRIAL)
    {
        return_from_time_trial = false;
        skip_music_tick = true;
        total_tracks = static_cast<int>(config.sound.music.size());

        if (pending_music_selected < 0 || pending_music_selected >= total_tracks)
            pending_music_selected = 0;

        cursor_pos = pending_music_selected;
        music_selected = pending_music_selected;
        last_music_selected = pending_music_selected;

        // GS_INIT_MUSIC increments playcount before calling enable(). This is
        // the same play, not a second credit, so cancel that duplicate count.
        if (config.stats.playcount > 0)
            config.stats.playcount--;

        video.enabled = false;
        return;
    }

    // A cancelled course selection returns to the normal frontend without
    // changing cannonball_mode, so do not let a stale marker skip a later game.
    return_from_time_trial = false;
    skip_music_tick = false;

    oferrari.car_ctrl_active = false;
    video.clear_text_ram();
    osprites.disable_sprites();
    otraffic.disable_traffic();
    //edit jump table 3
    oinitengine.car_increment = 0;
    oferrari.car_inc_old      = 0;
    osprites.spr_cnt_main     = 0;
    osprites.spr_cnt_shadow   = 0;
    oroad.road_ctrl           = ORoad::ROAD_BOTH_P0;
    oroad.horizon_base        = ORoad::HORIZON_OFF;
    last_music_selected       = -1;
    preview_counter           = -20; // Delay before playing music
    ostats.time_counter       = config.sound.music_timer; // Move 30 seconds to timer countdown (note on the original roms this is 15 seconds)
    ostats.frame_counter      = ostats.frame_reset;  
     
    blit_music_select();
    ohud.blit_text2(TEXT2_SELECT_MUSIC); // Select Music By Steering
  
    osoundint.queue_sound(sound::RESET);
    if (!config.sound.preview)
        osoundint.queue_sound(sound::PCM_WAVE); // Wave Noises

    // Enable block of sprites
    entry_start = OSprites::SPRITE_ENTRIES - 0x10;    
    for (int i = entry_start; i < entry_start + 5; i++)
    {
        osprites.jump_table[i].init(i);
    }

    setup_sprite1();
    setup_sprite2();
    setup_sprite3();
    setup_sprite4();
    setup_sprite5();

    // Widescreen tiles need additional palette information copied over
    if (tile_patch->loaded && config.s16_x_off > 0)
    {
        video.tile_layer->patch_tiles(tile_patch);
        otiles.setup_palette_widescreen();
    }

    video.tile_layer->set_x_clamp(video.tile_layer->CENTRE);

    total_tracks = (int)config.sound.music.size();
    cursor_pos = track_from_steering(oinputs.steering_adjust);
    music_selected = cursor_pos;
    ffb_detent_spring_applied = -1;
    ffb_detent_target_applied = 0;

    if (outrun.cannonball_mode == Outrun::MODE_CONT)
        set_game_mode(Outrun::MODE_CONT);
    else if (outrun.cannonball_mode == Outrun::MODE_TTRIAL)
        set_game_mode(Outrun::MODE_TTRIAL);
    else
        set_game_mode(Outrun::MODE_ORIGINAL);

    // For a physical LOW/HIGH cabinet shifter, track the raw LOW contact. This
    // gives the music screen both edges of the lever movement reliably. Other
    // gear modes continue to use the logical gear state or button edges below.
    if (config.controls.gear == config.controls.GEAR_PRESS)
        menu_gear_state = input.is_pressed(Input::GEAR1);
    else
        menu_gear_state = oinputs.gear;

    menu_gear_initialized = true;
    draw_game_options();
}

void OMusic::disable()
{
    reset_music_detent_ffb();

    // Disable block of sprites
    for (int i = entry_start; i < entry_start + 5; i++)
    {
        osprites.jump_table[i].control &= ~OSprites::ENABLE;
    }

    video.tile_layer->set_x_clamp(video.tile_layer->RIGHT);

    // Restore original palette for widescreen tiles.
    if (config.s16_x_off > 0)
    {
        video.tile_layer->restore_tiles();
        otiles.setup_palette_tilemap();
    }

    video.enabled = false; // Turn screen off
}

// Music Selection Screen: Setup Radio Sprite
// Source: 0xCAF0
void OMusic::setup_sprite1()
{
    oentry *e = &osprites.jump_table[entry_start + 0];
    e->x = 28;
    e->y = 180;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xB0;
    e->addr = outrun.adr.sprite_radio;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup Equalizer Sprite
// Source: 0xCB2A
void OMusic::setup_sprite2()
{
    oentry *e = &osprites.jump_table[entry_start + 1];
    e->x = 4;
    e->y = 189;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xA7;
    e->addr = outrun.adr.sprite_eq;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup FM Radio Readout
// Source: 0xCB64
void OMusic::setup_sprite3()
{
    oentry *e = &osprites.jump_table[entry_start + 2];
    e->x = -8;
    e->y = 176;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0x87;
    e->addr = outrun.adr.sprite_fm_left;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup FM Radio Dial
// Source: 0xCB9E
void OMusic::setup_sprite4()
{
    oentry *e = &osprites.jump_table[entry_start + 3];
    e->x = 68;
    e->y = 181;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0x89;
    e->addr = outrun.adr.sprite_dial_left;
    osprites.map_palette(e);
}

// Music Selection Screen: Setup Hand Sprite
// Source: 0xCBD8
void OMusic::setup_sprite5()
{
    oentry *e = &osprites.jump_table[entry_start + 4];
    e->x = 21;
    e->y = 196;
    e->road_priority = 0xFF;
    e->priority = 0x1FE;
    e->zoom = 0x7F;
    e->pal_src = 0xAF;
    e->addr = outrun.adr.sprite_hand_left;
    osprites.map_palette(e);
}

// Check for mode, colour and start input during music selection screen
//
// Source: 0xB768
void OMusic::check_start()
{
    // Direct three-button cabinets select modes immediately. The traditional
    // single VIEW button cycles through the same three choices.
    if (input.has_pressed(Input::VIEW1))
        set_game_mode(Outrun::MODE_ORIGINAL);
    else if (input.has_pressed(Input::VIEW2))
        set_game_mode(Outrun::MODE_CONT);
    else if (input.has_pressed(Input::VIEW3))
        set_game_mode(Outrun::MODE_TTRIAL);
    else if (input.has_pressed(Input::VIEWPOINT))
        cycle_game_mode();

    // Use the shifter as a colour selector only on this screen. Read the real
    // cabinet LOW/HIGH contact directly so both lever directions are detected;
    // the ordinary driving gear state is deliberately not used for that mode.
    if (config.controls.gear == config.controls.GEAR_PRESS)
    {
        const bool low_now = input.is_pressed(Input::GEAR1);

        if (!menu_gear_initialized)
        {
            menu_gear_state = low_now;
            menu_gear_initialized = true;
        }
        else if (low_now != menu_gear_state)
        {
            // Moving into LOW steps backwards; moving into HIGH steps forwards.
            cycle_car_color(low_now ? -1 : 1);
            menu_gear_state = low_now;
        }
    }
    else if (config.controls.gear == config.controls.GEAR_SEPARATE)
    {
        if (input.has_pressed(Input::GEAR1))
            cycle_car_color(-1);
        else if (input.has_pressed(Input::GEAR2))
            cycle_car_color(1);
    }
    else if (config.controls.gear == config.controls.GEAR_BUTTON)
    {
        // A one-button shifter has no physical direction. The normal gear logic
        // has already toggled oinputs.gear this frame, so alternate the colour
        // direction in the same LOW/HIGH sense.
        if (input.has_pressed(Input::GEAR1))
            cycle_car_color(oinputs.gear ? 1 : -1);
    }
    else // GEAR_AUTO
    {
        // Automatic transmission normally has no shifter, but honour explicit
        // gear bindings if the player configured them for menu use.
        if (input.has_pressed(Input::GEAR1))
            cycle_car_color(-1);
        else if (input.has_pressed(Input::GEAR2))
            cycle_car_color(1);
    }

    if (!ostats.credits || !input.has_pressed(Input::START))
        return;

    // Persist the selected car colour when the player commits to a run rather
    // than writing config.xml on every shifter movement.
    config.save();

    // If cannonball_mode is already TIME TRIAL, the legacy frontend has just
    // performed its course selection and this is its normal music screen. Do
    // not send that path back to the course map. Only a Time Trial newly chosen
    // here needs to enter the selector.
    if (game_mode_selected == Outrun::MODE_TTRIAL &&
        outrun.cannonball_mode != Outrun::MODE_TTRIAL)
    {
        pending_music_selected = music_selected;
        return_from_time_trial = true;

        // The Time Trial selector provides its own ambience. Stop a custom WAV
        // preview before handing control to the frontend course map.
        cannonball::audio.clear_wav();
        osoundint.queue_sound(sound::FM_RESET);

        ologo.disable();
        disable();

        if (menu)
        {
            menu->start_time_trial_from_music();
            return;
        }

        // Defensive fallback for builds without a frontend menu object.
        return_from_time_trial = false;
        set_game_mode(Outrun::MODE_ORIGINAL);
    }

    outrun.cannonball_mode = game_mode_selected;

    if (game_mode_selected == Outrun::MODE_CONT)
        set_continuous_traffic_from_difficulty();

    // Music-select mode switching happens after boot(), so refresh the correct
    // high-score table now rather than leaving the table from the launch mode.
    // Time Trial owns a separate score table that was already loaded by TTrial.
    if (game_mode_selected != Outrun::MODE_TTRIAL)
        config.load_scores(game_mode_selected == Outrun::MODE_ORIGINAL);

    outrun.game_state = GS_INIT_GAME;
    ologo.disable();
    disable();
}

// Tick and Blit
void OMusic::tick()
{
    // Time Trial selected from this screen has already chosen its music. The
    // frontend restarted the engine after course selection, so skip the normal
    // second music screen and let GS_INIT_GAME use the remembered song.
    if (skip_music_tick)
    {
        skip_music_tick = false;
        ostats.frame_counter = ostats.frame_reset + 1;
        outrun.game_state = GS_INIT_GAME;
        video.enabled = false;
        return;
    }

    // Radio Sprite
    osprites.do_spr_order_shadows(&osprites.jump_table[entry_start + 0]);

    // Animated EQ Sprite (Cycle the graphical equalizer on the radio)
    oentry *e = &osprites.jump_table[entry_start + 1];
    e->reload++; // Increment palette entry
    e->pal_src = roms.rom0.read8((e->reload & 0x3E) >> 1 | MUSIC_EQ_PAL);
    osprites.map_palette(e);
    osprites.do_spr_order_shadows(e);

    // Draw appropriate FM station on radio, depending on steering setting
    // Draw Dial on radio, depending on steering setting
    e = &osprites.jump_table[entry_start + 2];
    oentry *dial = &osprites.jump_table[entry_start + 3];
    oentry *hand = &osprites.jump_table[entry_start + 4];

    // Determine Track Selection Logic
    if (total_tracks < 3) tick_original(e, dial, hand);
    else tick_enhanced(e, dial, hand);

    osprites.do_spr_order_shadows(e);
    osprites.do_spr_order_shadows(dial);
    osprites.do_spr_order_shadows(hand);;

    draw_game_options();

    // Enhancement: Preview Music On Sound Selection Screen
    if (config.sound.preview)
    {
        if (music_selected != last_music_selected)
        {
            if (preview_counter == 0 && last_music_selected != -1)
                osoundint.queue_sound(sound::FM_RESET);

            if (++preview_counter >= 10)
            {
                play_music();
                preview_counter = 0;
            }      
        }
    }
}

void OMusic::play_music(int index)
{
    if (index == -1) index = music_selected;

    next_track = &config.sound.music.at(index);

    switch (next_track->type)
    {
        case music_t::IS_YM_INT:
            cannonball::audio.clear_wav();
            osoundint.queue_sound(next_track->cmd);
            break;

        case music_t::IS_YM_EXT:
            cannonball::audio.clear_wav();
            roms.load_ym_data((config.data.res_path + next_track->filename).c_str());
            osoundint.queue_sound(next_track->cmd);
            break;

        case music_t::IS_WAV:
            cannonball::audio.load_audio((config.data.res_path + next_track->filename).c_str());
            break;
    }

    last_music_selected = index;
}

// Cycle music in continuous mode
void OMusic::cycle_music()
{
    if (++music_selected > 2) music_selected = 0;
    play_music();
}

// Original Version of Music Selection Screen With 3 Tracks. 
// Wheel Left = Track 0, Wheel Centre = Track 1, Wheel Right = Track 2
void OMusic::tick_original(oentry* fm, oentry* dial, oentry* hand)
{
    // Note tiles to append to left side of text
    const uint32_t NOTE_TILES1 = 0x8A7A8A7B;
    const uint32_t NOTE_TILES2 = 0x8A7C8A7D;

    // Steer Left
    if (oinputs.steering_adjust + 0x80 <= 0x55)
    {                
        set_hand(HAND_LEFT, fm, dial, hand);
        ohud.blit_text2(TEXT2_MAGICAL);
        video.write_text32(0x1105C0, NOTE_TILES1);
        video.write_text32(0x110640, NOTE_TILES2);
        music_selected = 0;
    }
    // Centre
    else if (oinputs.steering_adjust + 0x80 <= 0xAA)
    {
        set_hand(HAND_CENTRE, fm, dial, hand);
        ohud.blit_text2(TEXT2_BREEZE);
        video.write_text32(0x1105C6, NOTE_TILES1);
        video.write_text32(0x110646, NOTE_TILES2);
        music_selected = 1;
    }
    // Steer Right
    else
    {
        set_hand(HAND_RIGHT, fm, dial, hand);
        ohud.blit_text2(TEXT2_SPLASH);
        video.write_text32(0x1105C8, NOTE_TILES1);
        video.write_text32(0x110648, NOTE_TILES2);
        music_selected = 2;
    }
}

// Enhanced music selection: one physical wheel zone per available track.
void OMusic::tick_enhanced(oentry* fm, oentry* dial, oentry* hand)
{
    const bool analog_selector = input.analog && input.gamepad;

    if (analog_selector)
    {
        const int steering = static_cast<int>(oinputs.steering_adjust);
        const int mapped_track = track_from_steering(steering);

        if (mapped_track != cursor_pos && total_tracks > 1)
        {
            // A small hysteresis stops wheel noise from flickering between two
            // songs while sitting directly on a virtual selector boundary.
            const int HYSTERESIS = 2;

            if (mapped_track > cursor_pos)
            {
                const int boundary =
                    -127 +
                    ((cursor_pos + 1) * 255) /
                    total_tracks;

                if (steering >= boundary + HYSTERESIS)
                    cursor_pos = mapped_track;
            }
            else
            {
                const int boundary =
                    -127 +
                    (cursor_pos * 255) /
                    total_tracks;

                if (steering <= boundary - HYSTERESIS)
                    cursor_pos = mapped_track;
            }
        }
    }
    else
    {
        // Keyboard/D-pad input remains discrete: one press moves exactly one
        // track instead of the old held-direction repeat timer.
        if (input.has_pressed(Input::LEFT))
        {
            if (--cursor_pos < 0)
                cursor_pos = total_tracks - 1;
        }
        else if (input.has_pressed(Input::RIGHT))
        {
            if (++cursor_pos >= total_tracks)
                cursor_pos = 0;
        }
    }

    music_selected = cursor_pos;

    // The original radio artwork only contains three hand/dial frames. Use the
    // selected track's virtual position to choose the nearest visual frame while
    // the actual selector can have any number of physical FFB positions.
    const int selector_position = steering_for_track(cursor_pos);

    if (selector_position < -42)
        set_hand(HAND_LEFT, fm, dial, hand);
    else if (selector_position <= 42)
        set_hand(HAND_CENTRE, fm, dial, hand);
    else
        set_hand(HAND_RIGHT, fm, dial, hand);

    ohud.blit_text_big(11, config.sound.music.at(music_selected).title.c_str(), true);
}

void OMusic::set_hand(short direction, oentry* fm, oentry* dial, oentry* hand)
{
    if (direction == HAND_LEFT)
    {                
        hand->x = 17;
        fm->addr   = outrun.adr.sprite_fm_left;
        dial->addr = outrun.adr.sprite_dial_left;
        hand->addr = outrun.adr.sprite_hand_left;
    }
    // Centre
    else if (direction == HAND_CENTRE)
    {
        hand->x = 21;
        fm->addr   = outrun.adr.sprite_fm_centre;
        dial->addr = outrun.adr.sprite_dial_centre;
        hand->addr = outrun.adr.sprite_hand_centre;
    }
    // Steer Right
    else if (direction == HAND_RIGHT)
    {
        hand->x = 21;
        fm->addr   = outrun.adr.sprite_fm_right;
        dial->addr = outrun.adr.sprite_dial_right;
        hand->addr = outrun.adr.sprite_hand_right;
    }
}

// Blit Only: Used when frame skipping
void OMusic::blit()
{
    for (int i = 0; i < 5; i++)
        osprites.do_spr_order_shadows(&osprites.jump_table[entry_start + i]);
}

// Blit Music Selection Tiles to text ram layer (Double Row)
// 
// Source Address: 0xE0DC
// Input:          Destination address into tile ram
// Output:         None
//
// Tilemap data is stored in the ROM as a series of words.
//
// A basic compression format is used:
//
// 1/ If a word is not '0000', copy it directly to tileram
// 2/ If a word is '0000' a long follows which details the compression.
//    The upper word of the long is the value to copy.
//    The lower word of the long is the number of times to copy that value.
//
// Tile structure:
//
// MSB          LSB
// ---nnnnnnnnnnnnn Tile index (0-8191)
// ---ccccccc------ Palette (0-127)
// p--------------- Priority flag
// -??------------- Unknown

void OMusic::blit_music_select()
{
    const uint32_t TILEMAP_RAM_16 = 0x10F030;

    // Palette Ram: 1F Long Entries For Sky Shade On Horizon, For Colour Change Effect
    const uint32_t PAL_RAM_SKY = 0x120F00;

    uint32_t src_addr = PAL_MUSIC_SELECT;
    uint32_t dst_addr = PAL_RAM_SKY;

    // Write 32 Palette Longs to Palette RAM
    for (int i = 0; i < 32; i++)
        video.write_pal32(&dst_addr, roms.rom0.read32(&src_addr));

    // Set Tilemap Scroll
    otiles.set_scroll(config.s16_x_off);
    
    // --------------------------------------------------------------------------------------------
    // Blit to Tilemap 16: Widescreen Version. Uses Custom Tilemap. 
    // --------------------------------------------------------------------------------------------
    if (tilemap->loaded && config.s16_x_off > 0)
    {
        uint32_t tilemap16 = TILEMAP_RAM_16 - 20;
        src_addr = 0;

        const uint16_t rows = tilemap->read16(&src_addr);
        const uint16_t cols = tilemap->read16(&src_addr);

        for (int y = 0; y < rows; y++)
        {
            dst_addr = tilemap16;
            for (int x = 0; x < cols; x++)
                video.write_tile16(&dst_addr, tilemap->read16(&src_addr));
            tilemap16 += 0x80; // next line of tiles
        }
    }
    // --------------------------------------------------------------------------------------------
    // Blit to Tilemap 16: Original 4:3 Version. 
    // --------------------------------------------------------------------------------------------
    else
    {
        uint32_t tilemap16 = TILEMAP_RAM_16;
        src_addr = TILEMAP_MUSIC_SELECT;

        for (int y = 0; y < 28; y++)
        {
            dst_addr = tilemap16;
            for (int x = 0; x < 40;)
            {
                // get next tile
                uint32_t data = roms.rom0.read16(&src_addr);
                // No Compression: write tile directly to tile ram
                if (data != 0)
                {
                    video.write_tile16(&dst_addr, data);    
                    x++;
                }
                // Compression
                else
                {
                    uint16_t value = roms.rom0.read16(&src_addr); // tile index to copy
                    uint16_t count = roms.rom0.read16(&src_addr); // number of times to copy value

                    for (uint16_t i = 0; i <= count; i++)
                    {
                        video.write_tile16(&dst_addr, value);
                        x++;
                    }
                }
            }
            tilemap16 += 0x80; // next line of tiles
        } // end for

        // Fix Misplaced tile on music select screen (above steering wheel)
        if (config.engine.fix_bugs)
            video.write_tile16(0x10F730, 0x0C80);
    } 
}