#pragma once

#include <SDL.h>

namespace car_palette_hotkey
{
    inline bool pressed()
    {
        static bool was_down = false;

        const Uint8* keyboard_state = SDL_GetKeyboardState(nullptr);
        const bool down =
            keyboard_state && keyboard_state[SDL_SCANCODE_F10] != 0;
        const bool trigger = down && !was_down;

        was_down = down;
        return trigger;
    }
}
