/***************************************************************************
    Front End Menu System - CannonBall-SE control-binding extensions.

    The existing menu implementation is retained in menu_base.cpp. Menu derives
    from MenuBase and replaces the sequential binding wizard with an editable
    KEYBOARD / GAMEPAD / WHEEL binding matrix.
***************************************************************************/

// Pre-include dependencies before the temporary class-name macro.
#include "main.hpp"
#include "menu.hpp"
#include "menulabels.hpp"
#include "../utils.hpp"

#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/osprites.hpp"
#include "engine/ologo.hpp"
#include "engine/omusic.hpp"
#include "engine/opalette.hpp"
#include "engine/otiles.hpp"

#include "frontend/cabdiag.hpp"
#include "frontend/ttrial.hpp"
#include "sdl2/gamepad_rumble_state.hpp"

#include <iostream>
#include "directx/ffeedback.hpp"
#include <string>
#include <string_view>
#include <cctype>
#include <algorithm>

// Compile the existing Menu implementation as MenuBase. Calls to virtual
// hooks from MenuBase dispatch to Menu below.
#define Menu MenuBase
#include "menu_base.cpp"
#undef Menu

namespace
{
    const int BINDING_ROWS = 12;
    const int BACK_ROW = BINDING_ROWS;
    const int EDITOR_ROWS = BINDING_ROWS + 1;
    const int EDITOR_COLUMNS = 3;

    const int COL_KEYBOARD = 0;
    const int COL_GAMEPAD = 1;
    const int COL_WHEEL = 2;

    const char* GAMEPAD_RUMBLE_LABEL = "GAMEPAD RUMBLE ";
    const char* PIXEL_SCALER_LABEL = "PIXEL SCALER ";

    bool starts_with_label(const std::string& value, const char* label)
    {
        return value.rfind(label, 0) == 0;
    }

    std::string gamepad_rumble_menu_text()
    {
        return std::string(GAMEPAD_RUMBLE_LABEL) +
            (gamepad_rumble::enabled ? "ON" : "OFF");
    }

    std::string pixel_scaler_menu_text()
    {
        return std::string(PIXEL_SCALER_LABEL) +
            pixel_scaler::name(
                pixel_scaler::mode.load(std::memory_order_relaxed));
    }

    const char* ROW_LABELS[BINDING_ROWS] =
    {
        "STEERING",
        "ACCELERATE",
        "BRAKE",
        "GEAR LOW",
        "GEAR HIGH",
        "START",
        "COIN",
        "MENU",
        "VIEW CHANGE",
        "VIEW 1",
        "VIEW 2",
        "VIEW 3",
    };

    const int ROW_TARGETS[BINDING_ROWS] =
    {
        device_binding_t::TARGET_STEER,
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

    // Steering is a two-key cell and therefore uses -1 here. The remaining
    // rows map directly to keyconfig[].
    const int ROW_KEY_SLOT[BINDING_ROWS] =
    {
        -1,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
    };

    std::string clip_text(const std::string& text, size_t width)
    {
        if (text.size() <= width)
            return text;

        return text.substr(0, width);
    }

    std::string compact_key_name(int key)
    {
        if (key < 0)
            return "-";

        switch (key)
        {
            case SDLK_UP:        return "UP";
            case SDLK_DOWN:      return "DOWN";
            case SDLK_LEFT:      return "LEFT";
            case SDLK_RIGHT:     return "RIGHT";
            case SDLK_RETURN:    return "ENTER";
            case SDLK_BACKSPACE: return "BSP";
            case SDLK_DELETE:    return "DEL";
            case SDLK_SPACE:     return "SPACE";
            case SDLK_ESCAPE:    return "ESC";
            default:
                break;
        }

        const char* name = SDL_GetKeyName(key);
        return (name && *name) ? std::string(name) : std::string("?");
    }

    std::string keyboard_binding_text(int row)
    {
        if (row == 0)
        {
            const std::string left = compact_key_name(config.controls.keyconfig[2]);
            const std::string right = compact_key_name(config.controls.keyconfig[3]);
            return clip_text(left + "/" + right, 8);
        }

        const int slot = ROW_KEY_SLOT[row];
        return clip_text(compact_key_name(config.controls.keyconfig[slot]), 8);
    }

    std::string hat_direction(int value)
    {
        if (value & SDL_HAT_UP)    return "U";
        if (value & SDL_HAT_DOWN)  return "D";
        if (value & SDL_HAT_LEFT)  return "L";
        if (value & SDL_HAT_RIGHT) return "R";
        return "?";
    }

    bool binding_is_group(const device_binding_t& binding, int group)
    {
        if (binding.device.size() < 2 || binding.device[1] != ':')
            return false;

        if (group == Input::BINDING_GAMEPAD)
            return binding.device[0] == 'G';

        return binding.device[0] == 'W';
    }

    std::string format_physical_binding(const device_binding_t& binding)
    {
        std::string text;

        switch (binding.type)
        {
            case device_binding_t::TYPE_AXIS:
                text = "AX" + std::to_string(binding.index);
                break;

            case device_binding_t::TYPE_HAT:
                text = "H" + std::to_string(binding.index) +
                    hat_direction(binding.value);
                break;

            default:
                text = "B" + std::to_string(binding.index);
                break;
        }

        return clip_text(text, 7);
    }

    std::string group_binding_text(int target, int group)
    {
        const device_binding_t* first = nullptr;
        int count = 0;

        for (const auto& binding : config.controls.device_bindings)
        {
            if (binding.target != target || !binding_is_group(binding, group))
                continue;

            if (!first)
                first = &binding;

            count++;
        }

        if (count == 0)
            return "-";

        if (count > 1)
            return "MULTI";

        return format_physical_binding(*first);
    }
}

void Menu::tick()
{
    // The original frontend uses analog steering as a menu up/down control.
    // That is convenient on a cabinet but extremely annoying with a PC wheel.
    // Neutralise steering only while browsing normal menus. In-game steering
    // and the binding editor itself continue to receive the real wheel value.
    const int16_t steering_before = oinputs.input_steering;

    if (state == STATE_MENU)
        oinputs.input_steering = 0x80;

    MenuBase::tick();
    oinputs.input_steering = steering_before;
}

void Menu::populate_controls()
{
    // Probe the GameController path before the base menu decides whether the
    // rumble options should be visible. enable=false guarantees no vibration.
    input.set_rumble(false, config.controls.rumble, 0);

    // Use the short label requested by the new unified binding editor and put
    // it first whenever the Controls menu is rebuilt.
    ENTRY_REDEFJOY = "CONFIG INPUTS";
    MenuBase::populate_controls();

    // Rumble now has a real enable switch. Strength remains a separate setting
    // and no longer doubles as an OFF state, so disabling it preserves level.
    const auto rumble_strength = std::find_if(
        menu_controls.begin(),
        menu_controls.end(),
        [](const std::string& entry)
        {
            return starts_with_label(entry, ENTRY_RUMBLE);
        });

    if (rumble_strength != menu_controls.end())
        menu_controls.insert(rumble_strength, gamepad_rumble_menu_text());

    const auto it = std::find(
        menu_controls.begin(),
        menu_controls.end(),
        std::string(ENTRY_REDEFJOY));

    if (it != menu_controls.end() && it != menu_controls.begin())
    {
        const std::string entry = *it;
        menu_controls.erase(it);
        menu_controls.insert(menu_controls.begin(), entry);
    }
}

bool Menu::select_pressed()
{
    // RETURN is a permanent frontend confirm key. Use an edge rather than
    // key_press so holding the key cannot confirm several nested menus at once.
    static bool return_was_down = false;

    const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
    const bool return_down =
        keyboard_state && keyboard_state[SDL_SCANCODE_RETURN] != 0;
    const bool return_pressed = return_down && !return_was_down;

    return_was_down = return_down;

    const bool pressed = return_pressed || MenuBase::select_pressed();
    if (!pressed)
        return false;

    if (menu_selected == &menu_enhancements &&
        cursor >= 0 &&
        cursor < static_cast<int>(menu_enhancements.size()))
    {
        const std::string& option = menu_enhancements[cursor];

        if (starts_with_label(option, PIXEL_SCALER_LABEL))
        {
            pixel_scaler::cycle();
            menu_enhancements[cursor] = pixel_scaler_menu_text();
            return false;
        }
    }

    if (menu_selected == &menu_handling &&
        cursor >= 0 &&
        cursor < static_cast<int>(menu_handling.size()))
    {
        const std::string& option = menu_handling[cursor];

        if (starts_with_label(option, ENTRY_COLOR))
        {
            config.engine.car_pal++;
            if (config.engine.car_pal > 7)
                config.engine.car_pal = 0;

            refresh_menu();
            return false;
        }
    }

    if (menu_selected == &menu_controls &&
        cursor >= 0 &&
        cursor < static_cast<int>(menu_controls.size()))
    {
        const std::string& option = menu_controls[cursor];

        if (starts_with_label(option, GAMEPAD_RUMBLE_LABEL))
        {
            gamepad_rumble::enabled = !gamepad_rumble::enabled;

            // Stop immediately when switching off. When switching on this also
            // refreshes/probes the controller without producing a vibration.
            input.set_rumble(false, config.controls.rumble, 0);
            menu_controls[cursor] = gamepad_rumble_menu_text();
            return false;
        }

        if (starts_with_label(option, ENTRY_RUMBLE))
        {
            // With a separate ON/OFF switch, strength only cycles through real
            // levels instead of wrapping through the old OFF/zero state.
            config.controls.rumble += 0.25f;
            if (config.controls.rumble > 1.0f || config.controls.rumble <= 0.0f)
                config.controls.rumble = 0.25f;

            refresh_menu();
            return false;
        }
    }

    return true;
}

void Menu::redefine_joystick()
{
    enum WaitType
    {
        WAIT_NONE,
        WAIT_KEY,
        WAIT_BUTTON,
        WAIT_HAT,
    };

    static int selected_row = 0;
    static int selected_col = COL_KEYBOARD;
    static bool capturing = false;
    static int steering_key_step = 0;

    static bool waiting_release = false;
    static bool capture_after_release = false;
    static WaitType wait_type = WAIT_NONE;
    static SDL_Keycode wait_key = SDLK_UNKNOWN;
    static SDL_JoystickID wait_device = -1;
    static int wait_button = -1;
    static int wait_hat = -1;
    static int wait_hat_value = SDL_HAT_CENTERED;

    auto clear_latches = [&]()
    {
        input.key_press = -1;
        input.joy_button = -1;
        input.joy_button_device = -1;
        input.joy_hat = -1;
        input.joy_hat_value = SDL_HAT_CENTERED;
        input.joy_hat_device = -1;
        input.reset_axis_config();
    };

    auto leave_editor = [&]()
    {
        clear_latches();
        capturing = false;
        waiting_release = false;
        capture_after_release = false;
        steering_key_step = 0;
        wait_type = WAIT_NONE;
        redef_state = 0;
        state = STATE_MENU;
        refresh_menu();
    };

    // menu_base.cpp sets redef_state to zero every time this editor is opened.
    if (redef_state == 0)
    {
        // The first implementation stored bindings against individual device
        // columns. Convert those to the new logical GAMEPAD/WHEEL grouping.
        input.normalize_device_bindings();

        selected_row = 0;
        selected_col = COL_KEYBOARD;
        capturing = false;
        steering_key_step = 0;
        waiting_release = false;
        capture_after_release = false;
        wait_type = WAIT_NONE;
        clear_latches();
        redef_state = 1;
    }

    auto draw_editor = [&]()
    {
        ohud.blit_text_new(12, 2, "CONTROL BINDINGS", ohud.GREEN);

        ohud.blit_text_new(1, 4, "CONTROL", ohud.GREY);
        ohud.blit_text_new(
            14,
            4,
            "KEYBOARD",
            (selected_row != BACK_ROW && selected_col == COL_KEYBOARD)
                ? ohud.PINK : ohud.GREY);
        ohud.blit_text_new(
            24,
            4,
            "GAMEPAD",
            (selected_row != BACK_ROW && selected_col == COL_GAMEPAD)
                ? ohud.PINK : ohud.GREY);
        ohud.blit_text_new(
            33,
            4,
            "WHEEL",
            (selected_row != BACK_ROW && selected_col == COL_WHEEL)
                ? ohud.PINK : ohud.GREY);

        for (int row = 0; row < BINDING_ROWS; row++)
        {
            const int y = 6 + row;

            ohud.blit_text_new(
                1,
                y,
                ROW_LABELS[row],
                row == selected_row ? ohud.PINK : ohud.GREEN);

            const std::string key_text = keyboard_binding_text(row);
            const std::string gamepad_text =
                group_binding_text(ROW_TARGETS[row], Input::BINDING_GAMEPAD);
            const std::string wheel_text =
                group_binding_text(ROW_TARGETS[row], Input::BINDING_WHEEL);

            ohud.blit_text_new(
                14,
                y,
                key_text.c_str(),
                (row == selected_row && selected_col == COL_KEYBOARD)
                    ? ohud.PINK : ohud.GREEN);
            ohud.blit_text_new(
                24,
                y,
                gamepad_text.c_str(),
                (row == selected_row && selected_col == COL_GAMEPAD)
                    ? ohud.PINK : ohud.GREEN);
            ohud.blit_text_new(
                33,
                y,
                wheel_text.c_str(),
                (row == selected_row && selected_col == COL_WHEEL)
                    ? ohud.PINK : ohud.GREEN);
        }

        ohud.blit_text_new(
            18,
            19,
            "BACK",
            selected_row == BACK_ROW ? ohud.PINK : ohud.GREEN);

        if (waiting_release)
        {
            ohud.blit_text_new(11, 21, "RELEASE CONTROL", ohud.PINK);
        }
        else if (capturing)
        {
            if (selected_col == COL_KEYBOARD && selected_row == 0)
            {
                ohud.blit_text_new(
                    4,
                    21,
                    steering_key_step == 0
                        ? "PRESS STEERING LEFT KEY"
                        : "PRESS STEERING RIGHT KEY",
                    ohud.PINK);
            }
            else if (selected_col == COL_KEYBOARD)
            {
                ohud.blit_text_new(8, 21, "PRESS A KEY", ohud.PINK);
            }
            else if (selected_row == 0)
            {
                ohud.blit_text_new(5, 21, "MOVE STEERING AXIS", ohud.PINK);
            }
            else if (selected_row == 1 || selected_row == 2)
            {
                ohud.blit_text_new(2, 21, "MOVE AXIS OR PRESS BUTTON", ohud.PINK);
            }
            else
            {
                ohud.blit_text_new(5, 21, "PRESS BUTTON OR HAT", ohud.PINK);
            }
        }
        else
        {
            ohud.blit_text_new(1, 21, "ARROWS - SELECT   ENTER - CHANGE", ohud.GREY);
            ohud.blit_text_new(1, 22, "DEL/BSP - CLEAR", ohud.GREY);
            ohud.blit_text_new(1, 23, "WHEEL - ALL RAW INPUT DEVICES", ohud.GREY);
        }
    };

    // Wait for the control that opened the cell or was just captured to be
    // released before listening again. This is what makes Enter bindable.
    if (waiting_release)
    {
        bool released = false;

        if (wait_type == WAIT_KEY)
        {
            const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
            const SDL_Scancode scancode = SDL_GetScancodeFromKey(wait_key);

            released =
                scancode == SDL_SCANCODE_UNKNOWN ||
                keyboard_state[scancode] == 0;
        }
        else if (wait_type == WAIT_BUTTON)
        {
            released =
                input.joy_button_device != wait_device ||
                input.joy_button != wait_button;
        }
        else if (wait_type == WAIT_HAT)
        {
            released =
                input.joy_hat_device != wait_device ||
                input.joy_hat != wait_hat ||
                (input.joy_hat_value & wait_hat_value) == 0;
        }
        else
        {
            released = true;
        }

        if (released)
        {
            waiting_release = false;
            wait_type = WAIT_NONE;
            wait_key = SDLK_UNKNOWN;
            wait_device = -1;
            wait_button = -1;
            wait_hat = -1;
            wait_hat_value = SDL_HAT_CENTERED;
            clear_latches();

            capturing = capture_after_release;
            capture_after_release = false;
        }

        draw_editor();
        return;
    }

    if (capturing)
    {
        if (selected_col == COL_KEYBOARD)
        {
            if (input.key_press != -1)
            {
                const SDL_Keycode captured_key = input.key_press;

                if (selected_row == 0)
                {
                    const int slot = steering_key_step == 0 ? 2 : 3;
                    config.controls.keyconfig[slot] = captured_key;

                    steering_key_step++;
                    capture_after_release = steering_key_step < 2;

                    if (!capture_after_release)
                        steering_key_step = 0;
                }
                else
                {
                    config.controls.keyconfig[ROW_KEY_SLOT[selected_row]] =
                        captured_key;
                    capture_after_release = false;
                }

                waiting_release = true;
                wait_type = WAIT_KEY;
                wait_key = captured_key;
                input.key_press = -1;
            }

            draw_editor();
            return;
        }

        const int group =
            selected_col == COL_GAMEPAD
                ? Input::BINDING_GAMEPAD
                : Input::BINDING_WHEEL;

        const SDL_JoystickID gamepad_device = input.get_gamepad_device();

        auto accepts_device = [&](SDL_JoystickID device)
        {
            if (device < 0)
                return false;

            // WHEEL intentionally accepts every raw SDL input device. GAMEPAD
            // remains restricted to the SDL GameController device.
            return group == Input::BINDING_WHEEL || device == gamepad_device;
        };

        const int target = ROW_TARGETS[selected_row];

        // Steering accepts an analog axis. Accelerator and brake accept either
        // an axis or a digital button/HAT. Other rows are digital controls.
        if (selected_row <= 2)
        {
            SDL_JoystickID captured_device = -1;
            const int axis = input.get_axis_config(&captured_device);

            if (axis != -1)
            {
                if (accepts_device(captured_device))
                {
                    input.set_device_binding(
                        target,
                        device_binding_t::TYPE_AXIS,
                        axis,
                        0,
                        captured_device,
                        group);

                    capturing = false;
                    clear_latches();
                }

                draw_editor();
                return;
            }
        }

        if (selected_row != 0 &&
            input.joy_hat != -1 &&
            input.joy_hat_value != SDL_HAT_CENTERED &&
            accepts_device(input.joy_hat_device))
        {
            const SDL_JoystickID captured_device = input.joy_hat_device;
            const int captured_hat = input.joy_hat;
            const int captured_value = input.joy_hat_value;

            input.set_device_binding(
                target,
                device_binding_t::TYPE_HAT,
                captured_hat,
                captured_value,
                captured_device,
                group);

            capturing = false;
            waiting_release = true;
            capture_after_release = false;
            wait_type = WAIT_HAT;
            wait_device = captured_device;
            wait_hat = captured_hat;
            wait_hat_value = captured_value;

            input.joy_hat = -1;
            input.joy_hat_value = SDL_HAT_CENTERED;
            input.joy_hat_device = -1;

            draw_editor();
            return;
        }

        if (selected_row != 0 &&
            input.joy_button != -1 &&
            accepts_device(input.joy_button_device))
        {
            const SDL_JoystickID captured_device = input.joy_button_device;
            const int captured_button = input.joy_button;

            input.set_device_binding(
                target,
                device_binding_t::TYPE_BUTTON,
                captured_button,
                0,
                captured_device,
                group);

            capturing = false;
            waiting_release = true;
            capture_after_release = false;
            wait_type = WAIT_BUTTON;
            wait_device = captured_device;
            wait_button = captured_button;

            input.joy_button = -1;
            input.joy_button_device = -1;

            draw_editor();
            return;
        }

        draw_editor();
        return;
    }

    // Browse mode: move through the fixed KEYBOARD / GAMEPAD / WHEEL matrix
    // and the final BACK entry.
    if (input.has_pressed(Input::MENU))
    {
        leave_editor();
        return;
    }

    if (input.has_pressed(Input::DOWN))
    {
        selected_row++;
        if (selected_row >= EDITOR_ROWS)
            selected_row = 0;

        if (selected_row == BACK_ROW)
            selected_col = COL_KEYBOARD;

        osoundint.queue_sound(sound::BEEP1);
    }
    else if (input.has_pressed(Input::UP))
    {
        selected_row--;
        if (selected_row < 0)
            selected_row = EDITOR_ROWS - 1;

        if (selected_row == BACK_ROW)
            selected_col = COL_KEYBOARD;

        osoundint.queue_sound(sound::BEEP1);
    }
    else if (selected_row != BACK_ROW && input.has_pressed(Input::RIGHT))
    {
        selected_col++;
        if (selected_col >= EDITOR_COLUMNS)
            selected_col = 0;

        osoundint.queue_sound(sound::BEEP1);
    }
    else if (selected_row != BACK_ROW && input.has_pressed(Input::LEFT))
    {
        selected_col--;
        if (selected_col < 0)
            selected_col = EDITOR_COLUMNS - 1;

        osoundint.queue_sound(sound::BEEP1);
    }

    const bool clear_pressed =
        input.key_press == SDLK_DELETE ||
        input.key_press == SDLK_BACKSPACE;

    if (clear_pressed && selected_row != BACK_ROW)
    {
        if (selected_col == COL_KEYBOARD)
        {
            if (selected_row == 0)
            {
                config.controls.keyconfig[2] = -1;
                config.controls.keyconfig[3] = -1;
            }
            else
            {
                config.controls.keyconfig[ROW_KEY_SLOT[selected_row]] = -1;
            }
        }
        else
        {
            const int group =
                selected_col == COL_GAMEPAD
                    ? Input::BINDING_GAMEPAD
                    : Input::BINDING_WHEEL;

            input.clear_device_bindings(ROW_TARGETS[selected_row], group);
        }

        input.key_press = -1;
        osoundint.queue_sound(sound::BEEP1);
    }

    // Call select_pressed first so its RETURN edge state is updated even when
    // input.key_press also contains RETURN. Otherwise BACK can immediately
    // re-open CONFIG INPUTS on the following menu frame while Return is held.
    const bool activate =
        select_pressed() ||
        input.key_press == SDLK_RETURN;

    if (activate)
    {
        if (selected_row == BACK_ROW)
        {
            osoundint.queue_sound(sound::BEEP1);
            leave_editor();
            return;
        }

        capturing = false;
        steering_key_step = 0;
        capture_after_release = true;

        if (input.joy_button != -1)
        {
            waiting_release = true;
            wait_type = WAIT_BUTTON;
            wait_device = input.joy_button_device;
            wait_button = input.joy_button;
        }
        else if (input.key_press != -1)
        {
            waiting_release = true;
            wait_type = WAIT_KEY;
            wait_key = input.key_press;
        }
        else
        {
            // Analog cabinet select has no discrete control to release.
            capturing = true;
            capture_after_release = false;
        }

        input.key_press = -1;
        input.joy_button = -1;
        input.joy_button_device = -1;
        input.joy_hat = -1;
        input.joy_hat_value = SDL_HAT_CENTERED;
        input.joy_hat_device = -1;
        input.reset_axis_config();
    }

    draw_editor();
}