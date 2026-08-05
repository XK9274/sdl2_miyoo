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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include "SDL_atomic.h"
#include "SDL_timer.h"
#include "../../events/SDL_events_c.h"
#include "../../thread/SDL_systhread.h"

#include "SDL_video_mmiyoo.h"
#include "SDL_event_mmiyoo.h"

MMIYOO_EventInfo MMiyooEventInfo = {0};

extern MMIYOO_VideoInfo MMiyooVideoInfo;

static SDL_atomic_t running;
static int event_fd = -1;
static uint32_t pre_ticks = 0;
static SDL_mutex *event_mutex = NULL;
static SDL_Thread *thread = NULL;
static uint32_t pre_keypad_bitmaps = 0;

static uint32_t keycode_to_miyoo_bit(int code)
{
    switch (code) {
    case KEY_UP:        return (1 << MYKEY_UP);
    case KEY_DOWN:      return (1 << MYKEY_DOWN);
    case KEY_LEFT:      return (1 << MYKEY_LEFT);
    case KEY_RIGHT:     return (1 << MYKEY_RIGHT);
    case KEY_SPACE:     return (1 << MYKEY_A);
    case KEY_LEFTCTRL:  return (1 << MYKEY_B);
    case KEY_LEFTSHIFT: return (1 << MYKEY_X);
    case KEY_LEFTALT:   return (1 << MYKEY_Y);
    case KEY_ENTER:     return (1 << MYKEY_START);
    case KEY_RIGHTCTRL: return (1 << MYKEY_SELECT);
    case KEY_E:         return (1 << MYKEY_L1);
    case KEY_TAB:       return (1 << MYKEY_L2);
    case KEY_T:         return (1 << MYKEY_R1);
    case KEY_BACKSPACE: return (1 << MYKEY_R2);
    case KEY_ESC:       return (1 << MYKEY_MENU);
    case KEY_POWER:     return (1 << MYKEY_POWER);
    case KEY_VOLUMEUP:  return (1 << MYKEY_VOLUP);
    case KEY_VOLUMEDOWN:return (1 << MYKEY_VOLDOWN);
    default:  return 0;
    }
}

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

int EventUpdate(void *data)
{
    struct input_event ev = {0};

    (void)data;

    while (SDL_AtomicGet(&running)) {
        if (event_fd >= 0) {
            ssize_t bytes = 0;

            while ((bytes = read(event_fd, &ev, sizeof(ev))) == sizeof(ev)) {
                if ((ev.type == EV_KEY) && (ev.value != 2)) {
                    const uint32_t bit = keycode_to_miyoo_bit(ev.code);

                    if (bit) {
                        SDL_LockMutex(event_mutex);
                        if (ev.value) {
                            MMiyooEventInfo.keypad.bitmaps |= bit;
                        } else {
                            MMiyooEventInfo.keypad.bitmaps &= ~bit;
                        }
                        if (!(MMiyooEventInfo.keypad.bitmaps & 0x0f)) {
                            pre_ticks = SDL_GetTicks();
                        }
                        SDL_UnlockMutex(event_mutex);
                    }
                }
            }

            if ((bytes < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) {
                usleep(1000000 / 60);
            }
        }
        usleep(1000000 / 60);
    }
    
    return 0;
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

#if defined(MMIYOO)
    event_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(event_fd < 0){
        printf("failed to open /dev/input/event0\n");
    }
#endif

    if((event_mutex =  SDL_CreateMutex()) == NULL) {
        SDL_SetError("Can't create input mutex");
        if(event_fd >= 0) {
            close(event_fd);
            event_fd = -1;
        }
        return;
    }

    SDL_AtomicSet(&running, 1);
    if((thread = SDL_CreateThreadInternal(EventUpdate, "MMIYOOInputThread", 4096, NULL)) == NULL) {
        SDL_SetError("Can't create input thread");
        SDL_AtomicSet(&running, 0);
        SDL_DestroyMutex(event_mutex);
        event_mutex = NULL;
        if(event_fd >= 0) {
            close(event_fd);
            event_fd = -1;
        }
        return;
    }
}

void MMIYOO_EventDeinit(void)
{
    SDL_AtomicSet(&running, 0);
    if(thread) {
        SDL_WaitThread(thread, NULL);
        thread = NULL;
    }
    if(event_mutex) {
        SDL_DestroyMutex(event_mutex);
        event_mutex = NULL;
    }
    if(event_fd >= 0) {
        close(event_fd);
        event_fd = -1;
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

    SDL_LockMutex(event_mutex);
    keypad_bitmaps = MMiyooEventInfo.keypad.bitmaps;
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
