/***************************************************************************
    XML Configuration File Handling.

    Load Settings.
    Load & Save Hi-Scores.

    Copyright Chris White.
    See license.txt for more details.

    This version for CannonBall-SE encorporates revisions that are
    Copyright (c) 2025 James Pearce:
    - Refactored to remove Boost dependencies using xml_loader.h shim and
      C++20 function equivalents
    - Automatic custom music loader
    - CRT effect handling settings
    - Play stats load/save
***************************************************************************/

#include <iostream>
#include <fstream>   // for std::ifstream / std::ofstream
#include <iterator>  // for std::istreambuf_iterator
#include <filesystem>
#include <regex>
#include <map>
#include <algorithm>
#include <cctype>
#include <string>
#include <cstdio>   // remove()

#include "main.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "../utils.hpp"

#include "engine/ohiscore.hpp"
#include "engine/outils.hpp"
#include "engine/audio/osoundint.hpp"


Config config;

Config::Config(void)
{
    data.cfg_file = "config.xml";

    // Setup default sounds
    music_t magical, breeze, splash;
    magical.title = "MAGICAL SOUND SHOWER";
    breeze.title  = "PASSING BREEZE";
    splash.title  = "SPLASH WAVE";
    magical.type  = music_t::IS_YM_INT;
    breeze.type   = music_t::IS_YM_INT;
    splash.type   = music_t::IS_YM_INT;
    magical.cmd   = sound::MUSIC_MAGICAL;
    breeze.cmd    = sound::MUSIC_BREEZE;
    splash.cmd    = sound::MUSIC_SPLASH;
    sound.music.push_back(magical); // 1st slot
    sound.music.push_back(breeze);  // 2nd slot
    sound.music.push_back(splash);  // 3rd slot
    // Users can replace these with custom music via .wav, .mp3, or .ym files in the res/ folder,
    // and/or add additional tracks.
    sound.custom_tracks_loaded = 0;
}

Config::~Config(void)
{
}

void Config::get_custom_music(const std::string& respath)
{
    namespace fs = std::filesystem;
#ifdef WITH_MP3
    static const std::map<std::string,int> ext_priority = {
        { "WAV", 0 },
        { "MP3", 1 },
        { "YM",  2 }
    };
#else
    static const std::map<std::string,int> ext_priority = {
        { "WAV", 0 },
        { "YM",  1 }
    };
#endif

    std::map<int, std::pair<std::string,fs::path>> chosen;
    std::regex pattern(R"((\d{2})[-_](.+))");

    for (auto& entry : fs::directory_iterator(respath)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        auto ext = path.extension().string();
        if (ext.size()<2) continue;
        ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
        auto prio_it = ext_priority.find(ext);
        if (prio_it == ext_priority.end()) continue;

        auto stem = path.stem().string();
        std::smatch m;
        if (!std::regex_match(stem, m, pattern)) continue;

        int idx = std::stoi(m[1]);      // track index 01–99
        int prio = prio_it->second;
        auto it = chosen.find(idx);
        if (it==chosen.end() || prio < ext_priority.at(it->second.first)) {
            chosen[idx] = { ext, path };
        }
    }

    // apply replacements/additions
    for (auto& [idx, ext_path] : chosen) {
        auto& [ext, filepath] = ext_path;

        // build display name in uppercase
        std::string raw = filepath.stem().string().substr(3); // drop "NN_"
        std::replace(raw.begin(), raw.end(), '_', ' ');
        std::replace(raw.begin(), raw.end(), '-', ' ');
        std::transform(raw.begin(), raw.end(), raw.begin(), ::toupper);

        // pick type based on extension
        int track_type = (ext == "YM")
            ? music_t::IS_YM_EXT
            : music_t::IS_WAV;   // WAV & MP3 both map to IS_WAV

        int cmd = sound::MUSIC_CUSTOM;

        std::cout << "Found music file " << raw;
        ++sound.custom_tracks_loaded;
        music_t entry {
            track_type,
            cmd,
            raw,                  // TITLE (upper‑case)
            filepath.filename().string()
        };

        if (idx >= 1 && idx <= 3) {
            std::cout << " (replacing built-in track " << idx << ")" << std::endl;
            // replace built‑in slot 0–2
            sound.music[idx-1] = entry;
        } else {
            std::cout << " (added as available track)" << std::endl;
            // append new track
            sound.music.push_back(entry);
        }
    }
}

// Set Path to load and save config to
void Config::set_config_file(const std::string& file)
{
    data.cfg_file = file;
}

void Config::load()
{
    cfg.clear();
    bool file_found = true;

    // Try current directory
    if (!xml_parser::read_xml(data.cfg_file, cfg)) {
        // settings not found. Flag that we need to create the file.
        file_found = false;
        // std::cerr << "Warning: " << data.cfg_file << " could not be loaded.\n";
        // Try res directory (which should contain a default configuration)
        std::string default_cfg_path = "res/" + data.cfg_file;
        if (!xml_parser::read_xml(default_cfg_path, cfg)) {
            std::cout << "Unable to load config.xml. Using defaults.";
            cfg.clear(); // reset the cfg ptree
        } else {
            std::cout << "Loaded settings from " << default_cfg_path << ".";
        }
        std::cout << " config.xml will be created in current directory.\n";
    }

    // ------------------------------------------------------------------------
    // Master Settings
    // ------------------------------------------------------------------------
    int F10Escape    = cfg.get_int("F10Escape", 0); // default is to use ESCAPE
    master_break_key = F10Escape ? SDLK_F10 : SDLK_ESCAPE;

    // ------------------------------------------------------------------------
    // Data Settings
    // ------------------------------------------------------------------------
    data.rom_path         = cfg.get_string("data.rompath", "roms/");  // Path to ROMs
    data.res_path         = cfg.get_string("data.respath", "res/");   // Path to resources
    data.save_path        = cfg.get_string("data.savepath", "./");    // Path to Save Data
    data.crc32            = cfg.get_int   ("data.crc32", 1);

    data.file_scores      = data.save_path + "hiscores.xml";
    data.file_scores_jap  = data.save_path + "hiscores_jap.xml";
    data.file_ttrial      = data.save_path + "hiscores_timetrial.xml";
    data.file_ttrial_jap  = data.save_path + "hiscores_timetrial_jap.xml";
    data.file_cont        = data.save_path + "hiscores_continuous.xml";
    data.file_cont_jap    = data.save_path + "hiscores_continuous_jap.xml";
    data.file_stats       = data.save_path + "play_stats.xml";

    // ------------------------------------------------------------------------
    // Menu Settings
    // ------------------------------------------------------------------------

    menu.enabled           = cfg.get_int("menu.enabled",   0);
    menu.road_scroll_speed = cfg.get_int("menu.roadspeed", 50);

    // ------------------------------------------------------------------------
    // Video Settings
    // ------------------------------------------------------------------------

    video.mode          = cfg.get_int("video.mode",            1); // Video Mode: Default is Full Screen
    video.scale         = cfg.get_int("video.window.scale",    1); // Video Scale: Default is 1x
    video.fps           = cfg.get_int("video.fps",             0); // Open game at 30fps; will auto-switch to 60fps if possible
    video.fps_count     = cfg.get_int("video.fps_counter",     0); // FPS Counter
    video.widescreen    = cfg.get_int("video.widescreen",      0); // Enable Widescreen Mode
    video.hires_next    =
    video.hires         = cfg.get_int("video.hires",           1); // Hi-Resolution Mode
    video.hiresprites   = cfg.get_int("video.hiresprites",     0); // enable hires sprites with hires mode
    video.vsync         = cfg.get_int("video.vsync",           1); // Use V-Sync where available (e.g. Open GL)
    video.x_offset      = cfg.get_int("video.x_offset",        0); // Offset from calculated image X position
    video.y_offset      = cfg.get_int("video.y_offset",        0); // Offset from calculated image Y position
    // JJP Additional configuration for CRT emulation
    video.shader_mode   = cfg.get_int("video.shader_mode",     2); // shader type: 0 = off (actually pass-through), 1 = fast, 2 = full
    video.shadow_mask   = cfg.get_int("video.shadow_mask",     2); // shadow mask type: 0 = off, 1 = overlay based (fast), 2 = shader based (looks better)
    video.mask_size     = cfg.get_int("video.mask_size",       3); // shadow mask size (3=normal, 4/5/6 for high DPI displays)
    video.maskDim       = cfg.get_int("video.maskDim",        75); // shadow mask type dim multiplier (75=75%)
    video.maskBoost     = cfg.get_int("video.maskBoost",     135); // shadow mask type boost multiplier (135=135%)
    video.scanlines     = cfg.get_int("video.scanlines",       0); // scanlines (0=off, 3=max)
    video.crt_shape     = cfg.get_int("video.crt_shape",       1); // CRT shape overlay on or off
    video.vignette      = cfg.get_int("video.vignette",       30); // amount to dim edges (1=1%)
    video.noise         = cfg.get_int("video.noise",           5); // amount of random noise to add (1=1%)
    video.warpX         = cfg.get_int("video.warpX",           3); // amount of warp to add along X axis (1=1%)
    video.warpY         = cfg.get_int("video.warpY",           4); // amount of warp to add along Y axis (1=1%)
    video.desaturate    = cfg.get_int("video.desaturate",      5); // amount to desaturate the entire image (raises black level) (1=1%)
    video.desaturate_edges = cfg.get_int("video.desaturate_edges", 4); // amount further desaturate towards edges (1=1%)
    video.brightboost   = cfg.get_int("video.brightboost",     0); // relative output brightness (1=1%)
    video.blargg        = cfg.get_int("video.blargg",          1); // Blargg filtering mode, 0=off
    video.saturation    = cfg.get_int("video.saturation",     30); // Blargg filter saturation, -1 to +1
    video.contrast      = cfg.get_int("video.contrast",        0); // Blargg filter contrast, -1 to +1
    video.brightness    = cfg.get_int("video.brightness",      0); // Blargg filter brightness, -1 to +1
    video.sharpness     = cfg.get_int("video.sharpness",       0); // Blargg edge bluring
    video.resolution    = cfg.get_int("video.resolution",      0); // Blargg resolution, -2 to 0
    video.gamma         = cfg.get_int("video.gamma",           0); // Blargg gamma, -3 to +3
    video.hue           = cfg.get_int("video.hue",            -2); // Blargg hue, -10 to +10 => -0.1 to +0.1

    // ------------------------------------------------------------------------
    // Sound Settings
    // ------------------------------------------------------------------------
    sound.enabled     = cfg.get_int("sound.enable",      1);
    sound.rate        = cfg.get_int("sound.rate",        44100);
    sound.advertise   = cfg.get_int("sound.advertise",   1);
    sound.preview     = cfg.get_int("sound.preview",     1);
    sound.fix_samples = cfg.get_int("sound.fix_samples", 1);
    sound.music_timer = cfg.get_int("sound.music_timer", 0);
    // JJP - allow either standard 8ms audio callbacks, or a slower 16ms rate (required with WSL2)
    sound.callback_rate   = cfg.get_int("sound.callback_rate",0);
    // Index of SDL playback device to request, -1 for default
    sound.playback_device = cfg.get_int("sound.playback_device", -1);

    // Custom Music. Search for enabled custom tracks
    get_custom_music(data.res_path);

    if (!sound.music_timer)
        sound.music_timer = MUSIC_TIMER;
    else
    {
        if (sound.music_timer > 99)
            sound.music_timer = 99;
        sound.music_timer = outils::DEC_TO_HEX[sound.music_timer]; // convert to hexadecimal
    }

    // JJP - Wave file playback volume, 1-8 where 4 = no adjustment
    sound.wave_volume = cfg.get_int("sound.wave_volume",4);

    // ------------------------------------------------------------------------
    // SMARTYPI Settings
    // ------------------------------------------------------------------------
    smartypi.enabled = cfg.get_int("smartypi.<xmlattr>.enabled",        0);
    smartypi.ouputs  = cfg.get_int("smartypi.outputs",                  1);
    smartypi.cabinet = cfg.get_int("smartypi.cabinet",                  1);

    // ------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------
    controls.gear          = cfg.get_int("controls.gear",               2);
    controls.steer_speed   = cfg.get_int("controls.steerspeed",         3);
    controls.pedal_speed   = cfg.get_int("controls.pedalspeed",         4);
    controls.rumble        = cfg.get_float("controls.rumble",           1.25f);
    controls.keyconfig[0]  = cfg.get_int("controls.keyconfig.up",       1073741906);
    controls.keyconfig[1]  = cfg.get_int("controls.keyconfig.down",     1073741905);
    controls.keyconfig[2]  = cfg.get_int("controls.keyconfig.left",     1073741904);
    controls.keyconfig[3]  = cfg.get_int("controls.keyconfig.right",    1073741903);
    controls.keyconfig[4]  = cfg.get_int("controls.keyconfig.acc",      97);
    controls.keyconfig[5]  = cfg.get_int("controls.keyconfig.brake",    122);
    controls.keyconfig[6]  = cfg.get_int("controls.keyconfig.gear1",    103);
    controls.keyconfig[7]  = cfg.get_int("controls.keyconfig.gear2",    104);
    controls.keyconfig[8]  = cfg.get_int("controls.keyconfig.start",    115);
    controls.keyconfig[9]  = cfg.get_int("controls.keyconfig.coin",     99);
    controls.keyconfig[10] = cfg.get_int("controls.keyconfig.menu",     109);
    controls.keyconfig[11] = cfg.get_int("controls.keyconfig.view",     118);
    controls.padconfig[0]  = cfg.get_int("controls.padconfig.acc",      -1);
    controls.padconfig[1]  = cfg.get_int("controls.padconfig.brake",    -1);
    controls.padconfig[2]  = cfg.get_int("controls.padconfig.gear1",    -1);
    controls.padconfig[3]  = cfg.get_int("controls.padconfig.gear2",    -1);
    controls.padconfig[4]  = cfg.get_int("controls.padconfig.start",    -1);
    controls.padconfig[5]  = cfg.get_int("controls.padconfig.coin",     -1);
    controls.padconfig[6]  = cfg.get_int("controls.padconfig.menu",     -1);
    controls.padconfig[7]  = cfg.get_int("controls.padconfig.view",     -1);
    controls.padconfig[8]  = cfg.get_int("controls.padconfig.up",       -1);
    controls.padconfig[9]  = cfg.get_int("controls.padconfig.down",     -1);
    controls.padconfig[10] = cfg.get_int("controls.padconfig.left",     -1);
    controls.padconfig[11] = cfg.get_int("controls.padconfig.right",    -1);
    controls.padconfig[12] = cfg.get_int("controls.padconfig.limit_l",  -1);
    controls.padconfig[13] = cfg.get_int("controls.padconfig.limit_c",  -1);
    controls.padconfig[14] = cfg.get_int("controls.padconfig.limit_r",  -1);
    controls.analog        = cfg.get_int("controls.analog.<xmlattr>.enabled", 1);
    controls.pad_id        = cfg.get_int("controls.pad_id",             0);
    controls.axis[0]       = cfg.get_int("controls.analog.axis.wheel",  -1);
    controls.axis[1]       = cfg.get_int("controls.analog.axis.accel",  -1);
    controls.axis[2]       = cfg.get_int("controls.analog.axis.brake",  -1);
    controls.axis[3]       = cfg.get_int("controls.analog.axis.motor",  -1);
    controls.axis_device[0] = cfg.get_string("controls.analog.axis_device.wheel", "");
    controls.axis_device[1] = cfg.get_string("controls.analog.axis_device.accel", "");
    controls.axis_device[2] = cfg.get_string("controls.analog.axis_device.brake", "");
    controls.axis_device[3] = cfg.get_string("controls.analog.axis_device.motor", "");
    controls.invert[1]     = cfg.get_int("controls.analog.axis.accel.<xmlattr>.invert", 0);
    controls.invert[2]     = cfg.get_int("controls.analog.axis.brake.<xmlattr>.invert", 0);
    controls.asettings[0]  = cfg.get_int("controls.analog.wheel.zone",  75);
    controls.asettings[1]  = cfg.get_int("controls.analog.wheel.dead",  0);

    controls.hat[0] = cfg.get_int("controls.hat.up.index", -1);
    controls.hat[1] = cfg.get_int("controls.hat.down.index", -1);
    controls.hat[2] = cfg.get_int("controls.hat.left.index", -1);
    controls.hat[3] = cfg.get_int("controls.hat.right.index", -1);

    controls.hat_value[0] = cfg.get_int("controls.hat.up.value", 0);
    controls.hat_value[1] = cfg.get_int("controls.hat.down.value", 0);
    controls.hat_value[2] = cfg.get_int("controls.hat.left.value", 0);
    controls.hat_value[3] = cfg.get_int("controls.hat.right.value", 0);

    controls.hat_device[0] = cfg.get_string("controls.hat.up.device", "");
    controls.hat_device[1] = cfg.get_string("controls.hat.down.device", "");
    controls.hat_device[2] = cfg.get_string("controls.hat.left.device", "");
    controls.hat_device[3] = cfg.get_string("controls.hat.right.device", "");

    controls.direction_custom[0] = cfg.get_int("controls.direction_custom.up", 0);
    controls.direction_custom[1] = cfg.get_int("controls.direction_custom.down", 0);
    controls.direction_custom[2] = cfg.get_int("controls.direction_custom.left", 0);
    controls.direction_custom[3] = cfg.get_int("controls.direction_custom.right", 0);

    controls.haptic        = cfg.get_int("controls.analog.haptic.<xmlattr>.enabled",    1);
    controls.ffb_strength  = cfg.get_int("controls.analog.haptic.strength", 50); 
    if (controls.ffb_strength < 10)
    controls.ffb_strength  = 10;
    else if (controls.ffb_strength > 100)
    controls.ffb_strength = 100;
    controls.centering_strength = cfg.get_int("controls.analog.haptic.centering_strength", 30);
    if (controls.centering_strength < 0)
        controls.centering_strength = 0;
    else if (controls.centering_strength > 100)
        controls.centering_strength = 100;

    // Internal FFB tuning values. These are not user-facing configuration options.
    controls.max_force      = 10000;
    controls.min_force      = 8500;
    controls.force_duration = 1;

    // Remove legacy FFB debug options from existing config files.
    cfg.erase("controls.analog.haptic.max_force");
    cfg.erase("controls.analog.haptic.min_force");
    cfg.erase("controls.analog.haptic.force_duration");

    // ------------------------------------------------------------------------
    // Engine Settings
    // ------------------------------------------------------------------------

    engine.dip_time      = cfg.get_int("engine.time",    0);
    engine.dip_traffic   = cfg.get_int("engine.traffic", 1);

    engine.freeze_timer    = engine.dip_time == 4;
    engine.disable_traffic = engine.dip_traffic == 4;
    engine.dip_time    &= 3;
    engine.dip_traffic &= 3;

    engine.freeplay      = cfg.get_int("engine.freeplay",        1) != 0;
    engine.jap           = cfg.get_int("engine.japanese_tracks", 0);
    engine.prototype     = cfg.get_int("engine.prototype",       0);

    // Additional Level Objects
    engine.level_objects   = cfg.get_int("engine.levelobjects",  1);
    engine.randomgen       = cfg.get_int("engine.randomgen",     1);
    engine.fix_bugs_backup = 
    engine.fix_bugs        = cfg.get_int("engine.fix_bugs",      1) != 0;
    engine.fix_timer       = cfg.get_int("engine.fix_timer",     0) != 0;
    engine.layout_debug    = cfg.get_int("engine.layout_debug",  0) != 0;
    engine.hiscore_delete  = cfg.get_int("scores.delete_last_entry", 1);
    engine.hiscore_timer   = cfg.get_int("scores.hiscore_timer", 0);
    engine.new_attract     = cfg.get_int("engine.new_attract",   1) != 0;
    engine.offroad         = cfg.get_int("engine.offroad",       0);
    engine.grippy_tyres    = cfg.get_int("engine.grippy_tyres",  0);
    engine.bumper          = cfg.get_int("engine.bumper",        0);
    engine.turbo           = cfg.get_int("engine.turbo",         0);
    engine.car_pal         = cfg.get_int("engine.car_color",     0);

    if (!engine.hiscore_timer)
        engine.hiscore_timer = HIGHSCORE_TIMER;
    else
    {
        if (engine.hiscore_timer > 99)
            engine.hiscore_timer = 99;
        engine.hiscore_timer = outils::DEC_TO_HEX[engine.hiscore_timer]; // convert to hexadecimal
    }

    // ------------------------------------------------------------------------
    // Time Trial Mode
    // ------------------------------------------------------------------------

    ttrial.laps    = cfg.get_int("time_trial.laps",    5);
    ttrial.traffic = cfg.get_int("time_trial.traffic", 3);
    cont_traffic   = cfg.get_int("continuous.traffic", 3);

    if (!file_found) {
        // create a config file with the defaults
        save();
    }
}

bool Config::save()
{
    // Update all the settings in the tree

    // Master Settings
    int F10Escape = (master_break_key == SDLK_F10) ? 1 : 0;
    cfg.put_int("F10Escape", F10Escape); // default is to use ESCAPE

    // JJP - CRT emulation settings
    cfg.put_int("video.mode",               video.mode);          // Video Mode: Full Screen (2)
    cfg.put_int("video.window.scale",       video.scale);         // Video Scale: 1x (1)
    cfg.put_int("video.fps_counter",        video.fps_count);     // FPS Counter (0)
    cfg.put_int("video.widescreen",         video.widescreen);    // Widescreen Mode (1)
    cfg.put_int("video.vsync",              video.vsync);         // V-Sync (1)
    cfg.put_int("video.hires",              video.hires);         // Game engine hires mode (1=enabled)
    cfg.put_int("video.hiresprites",        video.hiresprites);   // hi-res sprites (1=enabled)
    cfg.put_int("video.x_offset",           video.x_offset);      // X offset
    cfg.put_int("video.y_offset",           video.y_offset);      // Y offset
    // JJP Additional configuration for CRT emulation
    cfg.put_int("video.shader_mode",        video.shader_mode);   // Shader type (Off/Fast/Full) (2)
    cfg.put_int("video.shadow_mask",        video.shadow_mask);   // Shadow mask type (Off/Overlay/Shader) (2)
    cfg.put_int("video.mask_size",          video.mask_size);     // Shadow mask size (3)
    cfg.put_int("video.maskDim",            video.maskDim);       // Shadow mask Dim value (0)
    cfg.put_int("video.maskBoost",          video.maskBoost);     // Shadow mask Boost value (0)
    cfg.put_int("video.scanlines",          video.scanlines);     // Scanlines (0)
    cfg.put_int("video.crt_shape",          video.crt_shape);     // CRT shape overlay (0)
    cfg.put_int("video.vignette",           video.vignette);      // Vignette amount (0)
    cfg.put_int("video.noise",              video.noise);         // Noise amount (0)
    cfg.put_int("video.warpX",              video.warpX);         // Warp on X axis (0)
    cfg.put_int("video.warpY",              video.warpY);         // Warp on Y axis (0)
    cfg.put_int("video.desaturate",         video.desaturate);    // Desaturation level (0)
    cfg.put_int("video.desaturate_edges",   video.desaturate_edges); // Edge desaturation (0)
    cfg.put_int("video.brightboost",        video.brightboost);   // Bright boost element 1 (0)
    cfg.put_int("video.blargg",             video.blargg);        // Blargg filtering mode (0=off)
    cfg.put_int("video.saturation",         video.saturation);    // Filter saturation (-1 to +1)
    cfg.put_int("video.contrast",           video.contrast);      // Filter contrast (-1 to +1)
    cfg.put_int("video.brightness",         video.brightness);    // Filter brightness (-1 to +1)
    cfg.put_int("video.sharpness",          video.sharpness);     // Edge blurring
    cfg.put_int("video.resolution",         video.resolution);    // Resolution (-2 to 0)
    cfg.put_int("video.gamma",              video.gamma);         // Gamma (-3 to +3)
    cfg.put_int("video.hue",                video.hue);           // Hue (-10 to +10)

    cfg.put_int("sound.enable",             sound.enabled);
    cfg.put_int("sound.advertise",          sound.advertise);
    cfg.put_int("sound.preview",            sound.preview);
    cfg.put_int("sound.fix_samples",        sound.fix_samples);
    cfg.put_int("sound.rate",               sound.rate);             // audio sampling rate e.g. 44100 (Hz)
    cfg.put_int("sound.callback_rate",      sound.callback_rate);    // JJP - 0=8ms callbacks, 1=16ms
    cfg.put_int("sound.playback_device",    sound.playback_device);  // JJP - Index of SDL playback device to request, -1 for default  
    cfg.put_int("sound.wave_volume",        sound.wave_volume);      // JJP - volume adjustment to .wav files

    if (config.smartypi.enabled)
        cfg.put_int("smartypi.cabinet",     config.smartypi.cabinet);

    cfg.put_int("controls.gear",            controls.gear);
    cfg.put_float("controls.rumble",        controls.rumble);
    cfg.put_int("controls.steerspeed",      controls.steer_speed);
    cfg.put_int("controls.pedalspeed",      controls.pedal_speed);
    cfg.put_int("controls.keyconfig.up",    controls.keyconfig[0]);
    cfg.put_int("controls.keyconfig.down",  controls.keyconfig[1]);
    cfg.put_int("controls.keyconfig.left",  controls.keyconfig[2]);
    cfg.put_int("controls.keyconfig.right", controls.keyconfig[3]);
    cfg.put_int("controls.keyconfig.acc",   controls.keyconfig[4]);
    cfg.put_int("controls.keyconfig.brake", controls.keyconfig[5]);
    cfg.put_int("controls.keyconfig.gear1", controls.keyconfig[6]);
    cfg.put_int("controls.keyconfig.gear2", controls.keyconfig[7]);
    cfg.put_int("controls.keyconfig.start", controls.keyconfig[8]);
    cfg.put_int("controls.keyconfig.coin",  controls.keyconfig[9]);
    cfg.put_int("controls.keyconfig.menu",  controls.keyconfig[10]);
    cfg.put_int("controls.keyconfig.view",  controls.keyconfig[11]);
    cfg.put_int("controls.padconfig.acc",   controls.padconfig[0]);
    cfg.put_int("controls.padconfig.brake", controls.padconfig[1]);
    cfg.put_int("controls.padconfig.gear1", controls.padconfig[2]);
    cfg.put_int("controls.padconfig.gear2", controls.padconfig[3]);
    cfg.put_int("controls.padconfig.start", controls.padconfig[4]);
    cfg.put_int("controls.padconfig.coin",  controls.padconfig[5]);
    cfg.put_int("controls.padconfig.menu",  controls.padconfig[6]);
    cfg.put_int("controls.padconfig.view",  controls.padconfig[7]);
    cfg.put_int("controls.padconfig.up",    controls.padconfig[8]);
    cfg.put_int("controls.padconfig.down",  controls.padconfig[9]);
    cfg.put_int("controls.padconfig.left",  controls.padconfig[10]);
    cfg.put_int("controls.padconfig.right", controls.padconfig[11]);
    cfg.put_int("controls.analog.<xmlattr>.enabled", controls.analog);
    cfg.put_int("controls.analog.axis.wheel", controls.axis[0]);
    cfg.put_int("controls.analog.axis.accel", controls.axis[1]);
    cfg.put_int("controls.analog.axis.brake", controls.axis[2]);
    cfg.put_int("controls.analog.axis.motor", controls.axis[3]);

    cfg.put_string("controls.analog.axis_device.wheel", controls.axis_device[0]);
    cfg.put_string("controls.analog.axis_device.accel", controls.axis_device[1]);
    cfg.put_string("controls.analog.axis_device.brake", controls.axis_device[2]);
    cfg.put_string("controls.analog.axis_device.motor", controls.axis_device[3]);
    cfg.put_int("controls.analog.axis.accel.<xmlattr>.invert", controls.invert[1]);
    cfg.put_int("controls.analog.axis.brake.<xmlattr>.invert", controls.invert[2]);
    cfg.put_int("controls.analog.wheel.zone", controls.asettings[0]);
    cfg.put_int("controls.analog.wheel.dead", controls.asettings[1]);
    cfg.put_int("controls.analog.haptic.<xmlattr>.enabled", controls.haptic);
    cfg.put_int("controls.analog.haptic.strength", controls.ffb_strength);
    cfg.put_int("controls.analog.haptic.centering_strength", controls.centering_strength);


    cfg.put_int("controls.hat.up.index", controls.hat[0]);
    cfg.put_int("controls.hat.down.index", controls.hat[1]);
    cfg.put_int("controls.hat.left.index", controls.hat[2]);
    cfg.put_int("controls.hat.right.index", controls.hat[3]);

    cfg.put_int("controls.hat.up.value", controls.hat_value[0]);
    cfg.put_int("controls.hat.down.value", controls.hat_value[1]);
    cfg.put_int("controls.hat.left.value", controls.hat_value[2]);
    cfg.put_int("controls.hat.right.value", controls.hat_value[3]);

    cfg.put_string("controls.hat.up.device", controls.hat_device[0]);
    cfg.put_string("controls.hat.down.device", controls.hat_device[1]);
    cfg.put_string("controls.hat.left.device", controls.hat_device[2]);
    cfg.put_string("controls.hat.right.device", controls.hat_device[3]);

    cfg.put_int("controls.direction_custom.up", controls.direction_custom[0]);
    cfg.put_int("controls.direction_custom.down", controls.direction_custom[1]);
    cfg.put_int("controls.direction_custom.left", controls.direction_custom[2]);
    cfg.put_int("controls.direction_custom.right", controls.direction_custom[3]);

    cfg.put_int("engine.freeplay",        (int) engine.freeplay);
    cfg.put_int("engine.time",            engine.freeze_timer ? 4 : engine.dip_time);
    cfg.put_int("engine.traffic",         engine.disable_traffic ? 4 : engine.dip_traffic);
    cfg.put_int("engine.japanese_tracks", engine.jap);
    cfg.put_int("engine.prototype",       engine.prototype);
    cfg.put_int("engine.levelobjects",    engine.level_objects);
    cfg.put_int("engine.fix_bugs",        (int) engine.fix_bugs);
    cfg.put_int("engine.fix_timer",       (int) engine.fix_timer);
    cfg.put_int("engine.new_attract",     engine.new_attract);
    cfg.put_int("engine.offroad",         (int) engine.offroad);
    cfg.put_int("engine.grippy_tyres",    (int) engine.grippy_tyres);
    cfg.put_int("engine.bumper",          (int) engine.bumper);
    cfg.put_int("engine.turbo",           (int) engine.turbo);
    cfg.put_int("engine.car_color",       engine.car_pal);

    cfg.put_int("time_trial.laps",    ttrial.laps);
    cfg.put_int("time_trial.traffic", ttrial.traffic);
    cfg.put_int("continuous.traffic", cont_traffic);

    // Sync back from doc (mirrors original behavior)
    ttrial.laps    = cfg.get_int("time_trial.laps",    5);
    ttrial.traffic = cfg.get_int("time_trial.traffic", 3);
    cont_traffic   = cfg.get_int("continuous.traffic", 3);

    // Write out to the current directory (even if we loaded from res/)
    if (!xml_parser::write_xml(data.cfg_file, cfg)) {
        std::cerr << "Could not save settings to " << data.cfg_file << std::endl;
        return false;
    }
    return true;
}

void Config::load_scores(bool original_mode)
{
    std::string scores_file;

    if (original_mode)
        scores_file = engine.jap ? data.file_scores_jap : data.file_scores;
    else
        scores_file = engine.jap ? data.file_cont_jap : data.file_cont;

    xml_parser::ptree scores("scores");
    if (!xml_parser::read_xml(scores_file, scores)) {
        std::cerr << "Warning: " << scores_file << " could not be loaded." << std::endl;
        return;
    }

    // Game Scores
    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];

        std::string xmltag = "score";
        xmltag += Utils::to_string(i);

        e->score    = Utils::from_hex_string(
                        scores.get_string(xmltag + ".score",    "0"));
        e->initial1 =   scores.get_string(xmltag + ".initial1", ".")[0];
        e->initial2 =   scores.get_string(xmltag + ".initial2", ".")[0];
        e->initial3 =   scores.get_string(xmltag + ".initial3", ".")[0];
        e->maptiles = Utils::from_hex_string(
                        scores.get_string(xmltag + ".maptiles", "20202020"));
        e->time     = Utils::from_hex_string(
                        scores.get_string(xmltag + ".time"    , "0"));

        if (e->initial1 == '.') e->initial1 = 0x20;
        if (e->initial2 == '.') e->initial2 = 0x20;
        if (e->initial3 == '.') e->initial3 = 0x20;
    }
}

void Config::save_scores(bool original_mode)
{
    std::string scores_file;

    if (original_mode)
        scores_file = engine.jap ? data.file_scores_jap : data.file_scores;
    else
        scores_file = engine.jap ? data.file_cont_jap : data.file_cont;

    xml_parser::ptree scores("scores");

    for (int i = 0; i < ohiscore.NO_SCORES; i++)
    {
        score_entry* e = &ohiscore.scores[i];

        std::string xmltag = "score";
        xmltag += Utils::to_string(i);

        scores.put_string(xmltag + ".score",    Utils::to_hex_string(e->score));
        // '.' is used to represent space
        scores.put_string(xmltag + ".initial1", e->initial1 == 0x20 ? "." : Utils::to_string((char) e->initial1));
        scores.put_string(xmltag + ".initial2", e->initial2 == 0x20 ? "." : Utils::to_string((char) e->initial2));
        scores.put_string(xmltag + ".initial3", e->initial3 == 0x20 ? "." : Utils::to_string((char) e->initial3));
        scores.put_string(xmltag + ".maptiles", Utils::to_hex_string(e->maptiles));
        scores.put_string(xmltag + ".time",     Utils::to_hex_string(e->time));
    }

    if (!xml_parser::write_xml(scores_file, scores)) {
        std::cerr << "Could not save hiscores to: " << scores_file << std::endl;
    }
}

void Config::load_stats()
{
    std::string stats_file = data.file_stats;

    xml_parser::ptree stats_data("playstats");
    if (!xml_parser::read_xml(stats_file, stats_data)) {
        std::cerr << "Warning: " << stats_file << " could not be loaded." << std::endl;
        stats.playcount = 0;
        stats.runtime   = 0;
        return;
    }

    // Load machine stats from file
    stats.playcount = stats_data.get_int("playcount", 0);
    stats.runtime   = stats_data.get_int("runtime",   0);
}

void Config::save_stats()
{
    std::string stats_file = data.file_stats;

    xml_parser::ptree stats_data("playstats");

    stats_data.put_int("playcount", stats.playcount);
    stats_data.put_int("runtime",   stats.runtime);

    if (!xml_parser::write_xml(stats_file, stats_data)) {
        std::cerr << "Could not save machine stats to: " << stats_file << std::endl;
    }
}

void Config::load_timetrial_scores()
{
    // Counter value that represents 1m 15s 0ms
    static const uint16_t COUNTER_1M_15 = 0x11D0;

    std::string timetrial_file = engine.jap ? config.data.file_ttrial_jap : config.data.file_ttrial;
    xml_parser::ptree timetrial_scores("timetrial_scores");

    if (!xml_parser::read_xml(timetrial_file, timetrial_scores)) {
        std::cerr << "Warning: Could not load time-trial scores from: " << timetrial_file << std::endl;
        for (int i = 0; i < 15; i++)
            ttrial.best_times[i] = COUNTER_1M_15;
        return;
    }

    // Time Trial Scores
    for (int i = 0; i < 15; i++)
    {
        ttrial.best_times[i] = static_cast<uint16_t>(
                                    timetrial_scores.get_int("time_trial.score" + Utils::to_string(i), COUNTER_1M_15)
                               );
    }
}

void Config::save_timetrial_scores()
{
    std::string timetrial_file = engine.jap ? config.data.file_ttrial_jap : config.data.file_ttrial;
    xml_parser::ptree timetrial_scores("timetrial_scores");


    // Time Trial Scores
    for (int i = 0; i < 15; i++) {
        timetrial_scores.put_int("time_trial.score" + Utils::to_string(i), ttrial.best_times[i]);
    }


    if (!xml_parser::write_xml(timetrial_file, timetrial_scores)) {
        std::cerr << "Could not save time trial scores to: " << timetrial_file << std::endl;
    }
}

bool Config::clear_scores()
{
    // Init Default Hiscores
    ohiscore.init_def_scores();

    int deleted = 0;          // number of successful deletions

    auto try_remove = [&](const std::string& path) {
        if (std::remove(path.c_str()) == 0) ++deleted;
    };

    try_remove(data.file_scores);
    try_remove(data.file_scores_jap);
    try_remove(data.file_ttrial);
    try_remove(data.file_ttrial_jap);
    try_remove(data.file_cont);
    try_remove(data.file_cont_jap);

    // returns true if at least one file was deleted
    return (deleted > 0);
}

void Config::set_fps(int fps)
{
    video.fps = fps;
    // Set core FPS to 30fps or 60fps
    this->fps = video.fps == 0 ? 30 : 60;

    // Original game ticks sprites at 30fps but background scroll at 60fps
    tick_fps  = video.fps < 2 ? 30 : 60;

    cannonball::frame_ms = 1000.0 / this->fps;

    /* JJP - Sound initialised in seperate thread so not required here */
}

// Inc time setting from menu
void Config::inc_time()
{
    if (engine.dip_time == 3)
    {
        if (!engine.freeze_timer)
            engine.freeze_timer = 1;
        else
        {
            engine.dip_time = 0;
            engine.freeze_timer = 0;
        }
    }
    else
        engine.dip_time++;
}

// Inc traffic setting from menu
void Config::inc_traffic()
{
    if (engine.dip_traffic == 3)
    {
        if (!engine.disable_traffic)
            engine.disable_traffic = 1;
        else
        {
            engine.dip_traffic = 0;
            engine.disable_traffic = 0;
        }
    }
    else
        engine.dip_traffic++;
}