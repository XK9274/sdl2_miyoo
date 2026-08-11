/*
  Customized version for Miyoo-Mini handheld.
  Only tested under Miyoo-Mini stock OS (original firmware) with Parasyte compatible layer.

  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2022-2022 Steward Fu <steward.fu@gmail.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
    
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MMIYOO

#include "SDL_timer.h"
#include "../../events/SDL_events_c.h"

#include "SDL_video_mmiyoo.h"
#include "SDL_event_mmiyoo.h"

static SDL_mutex *event_mutex = NULL;
static uint32_t pre_keypad_bitmaps = 0;

void MMIYOO_EventInit(void)
{
    pre_keypad_bitmaps = 0;

    if((event_mutex =  SDL_CreateMutex()) == NULL) {
        SDL_SetError("Can't create input mutex");
        return;
    }

    MMIYOO_InputInit();
}

void MMIYOO_EventDeinit(void)
{
    MMIYOO_InputDeinit();
    if(event_mutex) {
        SDL_DestroyMutex(event_mutex);
        event_mutex = NULL;
    }
}

void MMIYOO_PumpEvents(_THIS)
{
    const SDL_Keycode code[]={
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
        SDLK_SPACE, SDLK_LCTRL, SDLK_LSHIFT, SDLK_LALT,
        SDLK_e, SDLK_t, SDLK_TAB, SDLK_BACKSPACE,
        SDLK_RCTRL, SDLK_RETURN, SDLK_ESCAPE
    };
    uint32_t keypad_bitmaps = 0;

    if (!event_mutex) {
        return;
    }

    keypad_bitmaps = MMIYOO_GetKeypadBitmap();

    if (!MMIYOO_IsKeyboardModeActive()) {
        /* Joystick mode is active -- MMIYOO_JoystickUpdate is the one
         * posting events from the shared bitmap instead. Keep the diff
         * baseline in sync so switching back to keyboard mode later doesn't
         * replay a burst of stale transitions. */
        pre_keypad_bitmaps = keypad_bitmaps;
        return;
    }

    /* SELECT held as a modifier: X = vsync off/adaptive toggle. Synthesizes
       the same SDLK_v hotkey the benchmarks already listen for (see
       controller_input.h BTN_VSYNC_TOGGLE), so nothing on the app side
       needs to change. SELECT's own key (RCTRL) still fires as a plain tap
       on release if no combo was used during the hold, matching each
       suite's existing "tap SELECT" handling (e.g. reset metrics) -- see
       bench_driver_translate_button_event's joystick-mode mirror of this
       same logic in common/driver_support.c. While SELECT is held,
       SELECT/X are masked out of keypad_bitmaps below so the generic
       per-bit loop doesn't also forward their normal keys (RCTRL/LSHIFT)
       to the game. */
    {
        static uint32_t select_was_held = 0;
        static uint32_t select_combo_used = 0;
        const uint32_t select_bit = keypad_bitmaps & (1u << MYKEY_SELECT);

        if (select_bit && !select_was_held) {
            select_combo_used = 0;
        }

        if (select_bit) {
            if (!select_combo_used && (keypad_bitmaps & (1u << MYKEY_X))) {
                select_combo_used = 1;
                SDL_SendKeyboardKey(SDL_PRESSED, SDL_GetScancodeFromKey(SDLK_v));
                SDL_SendKeyboardKey(SDL_RELEASED, SDL_GetScancodeFromKey(SDLK_v));
            }
        } else if (select_was_held && !select_combo_used) {
            SDL_SendKeyboardKey(SDL_PRESSED, SDL_GetScancodeFromKey(SDLK_RCTRL));
            SDL_SendKeyboardKey(SDL_RELEASED, SDL_GetScancodeFromKey(SDLK_RCTRL));
        }

        select_was_held = select_bit;

        if (select_bit) {
            keypad_bitmaps &= ~((1u << MYKEY_SELECT) | (1u << MYKEY_X));
        }
    }

    if (pre_keypad_bitmaps != keypad_bitmaps) {
        int cc = 0;
        uint32_t v0 = pre_keypad_bitmaps;
        uint32_t v1 = keypad_bitmaps;

        for (cc=0; cc<=MYKEY_LAST_BITS && cc < SDL_arraysize(code); cc++) {
            if ((v0 & 1) != (v1 & 1)) {
                SDL_SendKeyboardKey((v1 & 1) ? SDL_PRESSED : SDL_RELEASED, SDL_GetScancodeFromKey(code[cc]));
            }
            v0>>= 1;
            v1>>= 1;
        }
        pre_keypad_bitmaps = keypad_bitmaps;
    }
}

#endif
