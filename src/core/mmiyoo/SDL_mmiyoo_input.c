/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2026-2026 XK9274 <xk.github@pm.me>

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
#include "SDL.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <unistd.h>

#include "SDL_atomic.h"
#include "SDL_hints.h"
#include "SDL_log.h"
#include "SDL_mutex.h"
#include "SDL_thread.h"
#include "SDL_mmiyoo.h"
#include "../../thread/SDL_systhread.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

Uint32
MMIYOO_KeycodeToButtonMask(int code)
{
    switch (code) {
    case KEY_UP:
        return (1u << MMIYOO_BUTTON_UP);
    case KEY_DOWN:
        return (1u << MMIYOO_BUTTON_DOWN);
    case KEY_LEFT:
        return (1u << MMIYOO_BUTTON_LEFT);
    case KEY_RIGHT:
        return (1u << MMIYOO_BUTTON_RIGHT);
    case KEY_SPACE:
        return (1u << MMIYOO_BUTTON_A);
    case KEY_LEFTCTRL:
        return (1u << MMIYOO_BUTTON_B);
    case KEY_LEFTSHIFT:
        return (1u << MMIYOO_BUTTON_X);
    case KEY_LEFTALT:
        return (1u << MMIYOO_BUTTON_Y);
    case KEY_ENTER:
        return (1u << MMIYOO_BUTTON_START);
    case KEY_RIGHTCTRL:
        return (1u << MMIYOO_BUTTON_SELECT);
    case KEY_E:
        return (1u << MMIYOO_BUTTON_L1);
    case KEY_TAB:
        return (1u << MMIYOO_BUTTON_L2);
    case KEY_T:
        return (1u << MMIYOO_BUTTON_R1);
    case KEY_BACKSPACE:
        return (1u << MMIYOO_BUTTON_R2);
    case KEY_ESC:
        return (1u << MMIYOO_BUTTON_MENU);
    case KEY_POWER:
        return (1u << MMIYOO_BUTTON_POWER);
    case KEY_VOLUMEUP:
        return (1u << MMIYOO_BUTTON_VOLUP);
    case KEY_VOLUMEDOWN:
        return (1u << MMIYOO_BUTTON_VOLDOWN);
    default:
        return 0;
    }
}

/* Shared raw-input layer: a single reader of /dev/input/event0, consumed by
 * both the video backend (keyboard/mouse emulation) and the joystick
 * backend, instead of each independently opening and parsing the device.
 * Reference-counted so either (or both) can init/deinit in any order. */
static SDL_atomic_t s_input_ref_count;
static SDL_atomic_t s_input_running;
static int s_input_fd = -1;
static SDL_mutex *s_input_mutex = NULL;
static SDL_Thread *s_input_thread = NULL;
static Uint32 s_keypad_bitmap = 0;

static int
MMIYOO_InputThread(void *data)
{
    struct input_event ev = {0};

    (void)data;

    while (SDL_AtomicGet(&s_input_running)) {
        if (s_input_fd >= 0) {
            ssize_t bytes;

            while ((bytes = read(s_input_fd, &ev, sizeof(ev))) == sizeof(ev)) {
                if ((ev.type == EV_KEY) && (ev.value != 2)) {
                    const Uint32 bit = MMIYOO_KeycodeToButtonMask(ev.code);

                    if (bit) {
                        SDL_LockMutex(s_input_mutex);
                        if (ev.value) {
                            s_keypad_bitmap |= bit;
                        } else {
                            s_keypad_bitmap &= ~bit;
                        }
                        SDL_UnlockMutex(s_input_mutex);
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

void
MMIYOO_InputInit(void)
{
    if (SDL_AtomicIncRef(&s_input_ref_count) > 0) {
        return;
    }

    s_input_mutex = SDL_CreateMutex();
    if (!s_input_mutex) {
        return;
    }

#if defined(MMIYOO)
    s_input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_input_fd < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "MMIYOO_InputInit: failed to open /dev/input/event0: %s",
                    strerror(errno));
    }
#endif

    SDL_AtomicSet(&s_input_running, 1);
    s_input_thread = SDL_CreateThreadInternal(MMIYOO_InputThread, "MMIYOOInputThread", 4096, NULL);
    if (!s_input_thread) {
        SDL_AtomicSet(&s_input_running, 0);
        if (s_input_fd >= 0) {
            close(s_input_fd);
            s_input_fd = -1;
        }
    }
}

void
MMIYOO_InputDeinit(void)
{
    /* SDL_AtomicDecRef only returns true once the count truly reaches zero --
     * skip teardown while another owner (joystick or event backend) still
     * holds a reference. */
    if (!SDL_AtomicDecRef(&s_input_ref_count)) {
        return;
    }

    SDL_AtomicSet(&s_input_running, 0);
    if (s_input_thread) {
        SDL_WaitThread(s_input_thread, NULL);
        s_input_thread = NULL;
    }
    if (s_input_mutex) {
        SDL_DestroyMutex(s_input_mutex);
        s_input_mutex = NULL;
    }
    if (s_input_fd >= 0) {
        close(s_input_fd);
        s_input_fd = -1;
    }
}

Uint32
MMIYOO_GetKeypadBitmap(void)
{
    Uint32 bitmap = 0;

    if (s_input_mutex) {
        SDL_LockMutex(s_input_mutex);
        bitmap = s_keypad_bitmap;
        SDL_UnlockMutex(s_input_mutex);
    }

    return bitmap;
}

SDL_bool
MMIYOO_IsKeyboardModeActive(void)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_INPUT_MODE);
    if (mode) {
        return SDL_strcmp(mode, MMIYOO_INPUT_MODE_KEYBOARD) == 0 ? SDL_TRUE : SDL_FALSE;
    }
    /* No explicit hint: apps that never init the joystick subsystem have no
     * other way to receive input, so default them to synthesized keyboard
     * events. Apps that do call SDL_INIT_JOYSTICK (e.g. RetroArch) keep
     * getting real joystick events, unaffected by this default. */
    return SDL_WasInit(SDL_INIT_JOYSTICK) ? SDL_FALSE : SDL_TRUE;
}

SDL_bool
MMIYOO_IsJoystickModeActive(void)
{
    return MMIYOO_IsKeyboardModeActive() ? SDL_FALSE : SDL_TRUE;
}

/* vi: set ts=4 sw=4 expandtab: */
