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

MMIYOO_EventInfo MMiyooEventInfo = {0};

extern MMIYOO_VideoInfo MMiyooVideoInfo;

static uint32_t pre_ticks = 0;
static SDL_mutex *event_mutex = NULL;
static uint32_t pre_keypad_bitmaps = 0;

static void check_mouse_pos(void)
{
    if (MMiyooEventInfo.mouse.y < MMiyooEventInfo.mouse.miny) {
        MMiyooEventInfo.mouse.y = MMiyooEventInfo.mouse.miny;
    }
    if (MMiyooEventInfo.mouse.y > MMiyooEventInfo.mouse.maxy) {
        MMiyooEventInfo.mouse.y = MMiyooEventInfo.mouse.maxy;
    }
    if (MMiyooEventInfo.mouse.x < MMiyooEventInfo.mouse.minx) {
        MMiyooEventInfo.mouse.x = MMiyooEventInfo.mouse.minx;
    }
    if (MMiyooEventInfo.mouse.x >= MMiyooEventInfo.mouse.maxx) {
        MMiyooEventInfo.mouse.x = MMiyooEventInfo.mouse.maxx;
    }
}

static int get_move_interval(int type)
{
    float move = 0.0;

    move = ((float)(SDL_GetTicks() - pre_ticks)) / ((type == 0) ? 10.0f : 12.0f);
    if (move <= 0.0) {
        move = 1.0;
    }
    return (int)(1.0 * move);
}

void MMIYOO_EventInit(void)
{
    pre_keypad_bitmaps = 0;
    pre_ticks = SDL_GetTicks();
    memset(&MMiyooEventInfo, 0, sizeof(MMiyooEventInfo));
    MMiyooEventInfo.mouse.minx = 0;
    MMiyooEventInfo.mouse.miny = 0;
    MMiyooEventInfo.mouse.maxx = 256;
    MMiyooEventInfo.mouse.maxy = 192;
    MMiyooEventInfo.mouse.x = 50;
    MMiyooEventInfo.mouse.y = 190;
    MMiyooEventInfo.mode = MMIYOO_KEYPAD_MODE;

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

void MMIYOO_SetMouseBounds(int minx, int miny, int maxx, int maxy)
{
    if (event_mutex) {
        SDL_LockMutex(event_mutex);
    }
    MMiyooEventInfo.mouse.minx = minx;
    MMiyooEventInfo.mouse.miny = miny;
    MMiyooEventInfo.mouse.maxx = maxx;
    MMiyooEventInfo.mouse.maxy = maxy;
    check_mouse_pos();
    if (event_mutex) {
        SDL_UnlockMutex(event_mutex);
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
    int mode = MMIYOO_KEYPAD_MODE;
    int mouse_x = 0;
    int mouse_y = 0;
    int send_motion = 0;

    if (!event_mutex) {
        return;
    }

    keypad_bitmaps = MMIYOO_GetKeypadBitmap();
    if (!(keypad_bitmaps & 0x0f)) {
        pre_ticks = SDL_GetTicks();
    }

    if (!MMIYOO_IsKeyboardModeActive()) {
        /* Joystick mode is active -- MMIYOO_JoystickUpdate is the one
         * posting events from the shared bitmap instead. Keep the diff
         * baseline in sync so switching back to keyboard mode later doesn't
         * replay a burst of stale transitions. */
        pre_keypad_bitmaps = keypad_bitmaps;
        return;
    }

    SDL_LockMutex(event_mutex);
    mode = MMiyooEventInfo.mode;
    SDL_UnlockMutex(event_mutex);

    if (mode == MMIYOO_KEYPAD_MODE) {
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
    else {
        if (pre_keypad_bitmaps != keypad_bitmaps) {
            uint32_t v0 = pre_keypad_bitmaps;
            uint32_t v1 = keypad_bitmaps;

            if ((v0 & (1 << MYKEY_A)) != (v1 & (1 << MYKEY_A))) {
                SDL_SendMouseButton(MMiyooVideoInfo.window, 0, (v1 & (1 << MYKEY_A)) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
            }
        }

        SDL_LockMutex(event_mutex);
        if (keypad_bitmaps & (1 << MYKEY_UP)) {
            send_motion = 1;
            MMiyooEventInfo.mouse.y-= get_move_interval(1);
        }
        if (keypad_bitmaps & (1 << MYKEY_DOWN)) {
            send_motion = 1;
            MMiyooEventInfo.mouse.y+= get_move_interval(1);
        }
        if (keypad_bitmaps & (1 << MYKEY_LEFT)) {
            send_motion = 1;
            MMiyooEventInfo.mouse.x-= get_move_interval(0);
        }
        if (keypad_bitmaps & (1 << MYKEY_RIGHT)) {
            send_motion = 1;
            MMiyooEventInfo.mouse.x+= get_move_interval(0);
        }
        check_mouse_pos();
        mouse_x = MMiyooEventInfo.mouse.x;
        mouse_y = MMiyooEventInfo.mouse.y;
        SDL_UnlockMutex(event_mutex);

        if(send_motion){
            SDL_SendMouseMotion(MMiyooVideoInfo.window, 0, 0, mouse_x, mouse_y);
        }
        
        pre_keypad_bitmaps = keypad_bitmaps;
    }
}

#endif
