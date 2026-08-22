/***************************************************************************
    XML Configuration File Handling - CannonBall-SE extensions.

    The existing configuration implementation is retained in config_base.cpp.
    This wrapper adds persistent bindings for the three optional direct-view
    buttons and the per-device control-binding editor.
***************************************************************************/

// Pre-include the base file's dependencies before the temporary method-name
// macros below. This keeps the macros away from standard-library headers.
#include <iostream>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <regex>
#include <map>
#include <algorithm>
#include <cctype>
#include <string>
#include <cstdio>
#include <sstream>

#include "main.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "../utils.hpp"
#include "engine/car_palette_state.hpp"
#include "engine/ohiscore.hpp"
#include "engine/outils.hpp"
#include "engine/audio/osoundint.hpp"
#include "sdl2/gamepad_rumble_state.hpp"
#include "sdl2/pixel_scaler_state.hpp"

// Retain the existing Config::load/save implementation under private names.
#define load load_base
#define save save_base
#include "config_base.cpp"
#undef load
#undef save

namespace
{
    void add_legacy_binding(
        controls_settings_t& controls,
        int target,
        int type,
        int index,
        int value,
        const std::string& device)
    {
        if (index < 0)
            return;

        device_binding_t binding;
        binding.target = target;
        binding.type = type;
        binding.index = index;
        binding.value = value;
        binding.device = device.empty() ? "*" : device;
        controls.device_bindings.push_back(binding);
    }

    void migrate_legacy_device_bindings(controls_settings_t& controls)
    {
        // Analog controls already persisted a device signature, so those can
        // be migrated exactly. Old button bindings had no persisted device;
        // "*" deliberately preserves their previous any-device behaviour.
        add_legacy_binding(
            controls,
            device_binding_t::TARGET_STEER,
            device_binding_t::TYPE_AXIS,
            controls.axis[0],
            0,
            controls.axis_device[0]);

        add_legacy_binding(
            controls,
            device_binding_t::TARGET_ACCEL,
            device_binding_t::TYPE_AXIS,
            controls.axis[1],
            0,
            controls.axis_device[1]);

        add_legacy_binding(
            controls,
            device_binding_t::TARGET_BRAKE,
            device_binding_t::TYPE_AXIS,
            controls.axis[2],
            0,
            controls.axis_device[2]);

        static const int PAD_SLOT[] =
        {
            0,  // accelerate
            1,  // brake
            2,  // gear low / toggle
            3,  // gear high
            4,  // start
            5,  // coin
            6,  // menu
            7,  // view change
            15, // direct view 1
            16, // direct view 2
            17, // direct view 3
        };

        static const int TARGET[] =
        {
            device_binding_t::TARGET_ACCEL,
            device_binding_t::TARGET_BRAKE,
            device_binding_t::TARGET_GEAR1,
            device_binding_t::TARGET_GEAR2,
            device_binding_t::TARGET_START,
            device_binding_t::TARGET_COIN,
            device_binding_t::TARGET_MENU,
            device_binding_t::TARGET_VIEW,
            device_binding_t::TARGET_VIEW1,
            device_binding_t::TARGET_VIEW2,
            device_binding_t::TARGET_VIEW3,
        };

        for (int i = 0; i < 11; i++)
        {
            add_legacy_binding(
                controls,
                TARGET[i],
                device_binding_t::TYPE_BUTTON,
                controls.padconfig[PAD_SLOT[i]],
                0,
                "*");
        }
    }

    void disable_migrated_legacy_bindings(controls_settings_t& controls)
    {
        // Directional D-pad/HAT bindings (8-11) and cabinet motor limits
        // (12-14) stay in the original system for now. Everything represented
        // by the matrix is handled by device_bindings instead.
        controls.axis[0] = -1;
        controls.axis[1] = -1;
        controls.axis[2] = -1;
        controls.axis_device[0].clear();
        controls.axis_device[1].clear();
        controls.axis_device[2].clear();

        static const int PAD_SLOT[] =
        {
            0, 1, 2, 3, 4, 5, 6, 7, 15, 16, 17
        };

        for (int slot : PAD_SLOT)
            controls.padconfig[slot] = -1;
    }

    bool parse_device_bindings(
        const std::string& encoded,
        std::vector<device_binding_t>& bindings)
    {
        bindings.clear();

        if (encoded.empty())
            return false;

        std::stringstream entries(encoded);
        std::string entry;

        while (std::getline(entries, entry, ';'))
        {
            if (entry.empty())
                continue;

            std::stringstream fields(entry);
            std::string target;
            std::string type;
            std::string index;
            std::string value;
            std::string device;

            if (!std::getline(fields, target, ',') ||
                !std::getline(fields, type, ',') ||
                !std::getline(fields, index, ',') ||
                !std::getline(fields, value, ',') ||
                !std::getline(fields, device))
            {
                continue;
            }

            try
            {
                device_binding_t binding;
                binding.target = std::stoi(target);
                binding.type = std::stoi(type);
                binding.index = std::stoi(index);
                binding.value = std::stoi(value);
                binding.device = device;

                if (binding.target < device_binding_t::TARGET_STEER ||
                    binding.target > device_binding_t::TARGET_VIEW3 ||
                    binding.type < device_binding_t::TYPE_BUTTON ||
                    binding.type > device_binding_t::TYPE_HAT ||
                    binding.index < 0 ||
                    binding.device.empty())
                {
                    continue;
                }

                bindings.push_back(binding);
            }
            catch (...)
            {
                // Ignore malformed entries and keep loading the rest.
            }
        }

        return true;
    }

    std::string encode_device_bindings(
        const std::vector<device_binding_t>& bindings)
    {
        std::ostringstream encoded;
        bool first = true;

        for (const auto& binding : bindings)
        {
            if (binding.index < 0 || binding.device.empty())
                continue;

            if (!first)
                encoded << ';';

            first = false;
            encoded
                << binding.target << ','
                << binding.type << ','
                << binding.index << ','
                << binding.value << ','
                << binding.device;
        }

        return encoded.str();
    }
}

void Config::load()
{
    load_base();

    // engine.car_pal remains the live/runtime Ferrari colour. Keep a separate
    // persistent default so Music Select can change the race colour without
    // ever overwriting the user's attract/default colour.
    car_palette_state::initialize(engine.car_pal);
    engine.car_pal = car_palette_state::get_default(engine.car_pal);

    int scaler_mode = cfg.get_int("video.pixel_scaler", pixel_scaler::OFF);
    int scaler_last = cfg.get_int("video.pixel_scaler_last", pixel_scaler::XBRZ_4X);

    if (!pixel_scaler::valid(scaler_mode))
        scaler_mode = pixel_scaler::OFF;
    if (!pixel_scaler::active(scaler_last))
        scaler_last = pixel_scaler::XBRZ_4X;

    pixel_scaler::last_mode.store(scaler_last, std::memory_order_relaxed);
    pixel_scaler::set(scaler_mode);

    // Rumble enable is deliberately independent from rumble strength. Legacy
    // configs used strength 0 as OFF, so preserve that intent on first load.
    const bool legacy_rumble_enabled = controls.rumble > 0.0f;
    gamepad_rumble::enabled =
        cfg.get_int(
            "controls.rumble_enabled",
            legacy_rumble_enabled ? 1 : 0) != 0;

    // The old default could exceed the menu's 0..1 range. Keep a valid stored
    // strength even while rumble is disabled; the separate enable flag decides
    // whether the motors actually run.
    if (controls.rumble <= 0.0f)
        controls.rumble = 0.5f;
    else if (controls.rumble > 1.0f)
        controls.rumble = 1.0f;

    // Optional direct camera selection bindings. -1 means unassigned.
    controls.keyconfig[12] = cfg.get_int("controls.keyconfig.view1", -1);
    controls.keyconfig[13] = cfg.get_int("controls.keyconfig.view2", -1);
    controls.keyconfig[14] = cfg.get_int("controls.keyconfig.view3", -1);

    // Slots 12-14 remain the original cabinet motor-limit inputs.
    // The three new view buttons therefore use slots 15-17.
    controls.padconfig[15] = cfg.get_int("controls.padconfig.view1", -1);
    controls.padconfig[16] = cfg.get_int("controls.padconfig.view2", -1);
    controls.padconfig[17] = cfg.get_int("controls.padconfig.view3", -1);

    const std::string encoded =
        cfg.get_string("controls.device_bindings", "");

    if (!parse_device_bindings(encoded, controls.device_bindings))
    {
        migrate_legacy_device_bindings(controls);
    }

    disable_migrated_legacy_bindings(controls);
}

bool Config::save()
{
    cfg.put_int(
        "video.pixel_scaler",
        pixel_scaler::mode.load(std::memory_order_relaxed));
    cfg.put_int(
        "video.pixel_scaler_last",
        pixel_scaler::last_mode.load(std::memory_order_relaxed));

    // Keep the on/off state separate so switching rumble off never overwrites
    // the user's preferred intensity.
    cfg.put_int(
        "controls.rumble_enabled",
        gamepad_rumble::enabled ? 1 : 0);

    // Add the direct-view keyboard bindings to the same config tree before the
    // existing save routine writes it.
    cfg.put_int("controls.keyconfig.view1", controls.keyconfig[12]);
    cfg.put_int("controls.keyconfig.view2", controls.keyconfig[13]);
    cfg.put_int("controls.keyconfig.view3", controls.keyconfig[14]);

    // Per-device bindings supersede the old single pad/axis assignment for all
    // controls represented by the matrix.
    cfg.put_string(
        "controls.device_bindings",
        encode_device_bindings(controls.device_bindings));

    // A colour changed in the frontend settings menu is a real default change.
    // While the engine is running, car_pal is runtime state: Music Select and
    // the race are never allowed to promote that temporary value implicitly.
    if (cannonball::state != cannonball::STATE_GAME)
        car_palette_state::set_default(engine.car_pal);

    // config_base.cpp persists engine.car_pal as engine.car_color. Temporarily
    // substitute the persistent attract/default colour so a Music Select or
    // in-race colour can never leak into config.xml.
    const int runtime_car_pal = engine.car_pal;
    engine.car_pal = car_palette_state::get_default(runtime_car_pal);
    const bool saved = save_base();
    engine.car_pal = runtime_car_pal;

    return saved;
}
