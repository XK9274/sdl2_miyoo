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

#if defined(SDL_JOYSTICK_MMIYOO)

#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_joystick.h"
#include "neon.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"

#define MMIYOO_AXIS_MIN -32768
#define MMIYOO_AXIS_MAX 32767

static Uint32 button_state = 0;
static Uint32 previous_button_state = 0;
static Sint16 previous_axis_x = 0;
static Sint16 previous_axis_y = 0;

static int MMIYOO_JoystickInit(void)
{
    button_state = 0;
    previous_button_state = 0;
    previous_axis_x = 0;
    previous_axis_y = 0;

    MMIYOO_InputInit();

    return 1;
}

static int MMIYOO_JoystickGetCount(void)
{
    return 1;
}

static void MMIYOO_JoystickDetect(void)
{
}

static const char* MMIYOO_JoystickGetDeviceName(int device_index)
{
    return "MMiyoo Joystick";
}

static int MMIYOO_JoystickGetDevicePlayerIndex(int device_index)
{
    return -1;
}

static void MMIYOO_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_JoystickGUID MMIYOO_JoystickGetDeviceGUID(int device_index)
{
    SDL_JoystickGUID guid;
    const char *name = MMIYOO_JoystickGetDeviceName(device_index);
    SDL_zero(guid);
    neon_memcpy(&guid, name, SDL_min(sizeof(guid), SDL_strlen(name)));
    return guid;
}

static SDL_JoystickID MMIYOO_JoystickGetDeviceInstanceID(int device_index)
{
    return device_index;
}

static int MMIYOO_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    (void)device_index;

    joystick->nbuttons = MMIYOO_BUTTON_COUNT;
    joystick->naxes = 2;
    joystick->nhats = 0;

    MMIYOO_SetRumble(SDL_FALSE);
    return 0;
}

static int MMIYOO_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;

    return MMIYOO_SetRumble((low_frequency_rumble || high_frequency_rumble) ? SDL_TRUE : SDL_FALSE);
}

static int MMIYOO_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;

    return SDL_Unsupported();
}

static Uint32 MMIYOO_JoystickGetCapabilities(SDL_Joystick *joystick)
{
    (void)joystick;

    return MMIYOO_HasRumble() ? SDL_JOYCAP_RUMBLE : 0;
}

static int MMIYOO_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static int MMIYOO_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static int MMIYOO_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled)
{
    return SDL_Unsupported();
}

static void MMIYOO_JoystickUpdate(SDL_Joystick *joystick)
{
    Uint32 changed;
    Sint16 axis_x;
    Sint16 axis_y;
    SDL_bool left;
    SDL_bool right;
    SDL_bool up;
    SDL_bool down;
    int i;

    if (!MMIYOO_IsJoystickModeActive()) {
        /* Keyboard-emulation mode is active -- resync the diff baseline to
         * the real bitmap without posting anything, so reactivating later
         * diffs against the true current state rather than manufacturing a
         * false "just pressed" edge for whatever is already held. */
        previous_button_state = MMIYOO_GetKeypadBitmap();
        return;
    }

    button_state = MMIYOO_GetKeypadBitmap();

    changed = previous_button_state ^ button_state;
    if (changed) {
        static const char *button_names[MMIYOO_BUTTON_COUNT] = {
            "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y",
            "L1", "R1", "L2", "R2", "SELECT", "START", "MENU",
            "QSAVE", "QLOAD", "FF", "EXIT", "POWER", "VOLUP", "VOLDOWN"
        };
        for (i = 0; i < MMIYOO_BUTTON_COUNT; ++i) {
            const Uint32 bit = (1u << i);

            if (changed & bit) {
                if ((button_state & bit) && SDL_GetHintBoolean("SDL_MMIYOO_DEBUG_LOG", SDL_FALSE)) {
                    SDL_Log("BTNDBG press: %s", button_names[i]);
                }
                SDL_PrivateJoystickButton(joystick, (Uint8)i, (button_state & bit) ? SDL_PRESSED : SDL_RELEASED);
            }
        }
        previous_button_state = button_state;
    }

    left = (button_state & (1u << MMIYOO_BUTTON_LEFT)) ? SDL_TRUE : SDL_FALSE;
    right = (button_state & (1u << MMIYOO_BUTTON_RIGHT)) ? SDL_TRUE : SDL_FALSE;
    up = (button_state & (1u << MMIYOO_BUTTON_UP)) ? SDL_TRUE : SDL_FALSE;
    down = (button_state & (1u << MMIYOO_BUTTON_DOWN)) ? SDL_TRUE : SDL_FALSE;

    axis_x = 0;
    if (left && !right) {
        axis_x = MMIYOO_AXIS_MIN;
    } else if (right && !left) {
        axis_x = MMIYOO_AXIS_MAX;
    }

    axis_y = 0;
    if (up && !down) {
        axis_y = MMIYOO_AXIS_MIN;
    } else if (down && !up) {
        axis_y = MMIYOO_AXIS_MAX;
    }

    if (axis_x != previous_axis_x) {
        SDL_PrivateJoystickAxis(joystick, 0, axis_x);
        previous_axis_x = axis_x;
    }
    if (axis_y != previous_axis_y) {
        SDL_PrivateJoystickAxis(joystick, 1, axis_y);
        previous_axis_y = axis_y;
    }
}

static void MMIYOO_JoystickClose(SDL_Joystick *joystick)
{
    int i;

    for (i = 0; i < MMIYOO_BUTTON_COUNT; ++i) {
        if (button_state & (1u << i)) {
            SDL_PrivateJoystickButton(joystick, (Uint8)i, SDL_RELEASED);
        }
    }
    if (previous_axis_x != 0) {
        SDL_PrivateJoystickAxis(joystick, 0, 0);
    }
    if (previous_axis_y != 0) {
        SDL_PrivateJoystickAxis(joystick, 1, 0);
    }
    button_state = 0;
    previous_button_state = 0;
    previous_axis_x = 0;
    previous_axis_y = 0;
}

static void MMIYOO_JoystickQuit(void)
{
    MMIYOO_InputDeinit();
    button_state = 0;
    previous_button_state = 0;
    previous_axis_x = 0;
    previous_axis_y = 0;
    MMIYOO_SetRumble(SDL_FALSE);
}

static SDL_bool MMIYOO_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    (void)device_index;

    SDL_zero(*out);

    out->a.kind = EMappingKind_Button;
    out->a.target = MMIYOO_BUTTON_A;
    out->b.kind = EMappingKind_Button;
    out->b.target = MMIYOO_BUTTON_B;
    out->x.kind = EMappingKind_Button;
    out->x.target = MMIYOO_BUTTON_X;
    out->y.kind = EMappingKind_Button;
    out->y.target = MMIYOO_BUTTON_Y;
    out->back.kind = EMappingKind_Button;
    out->back.target = MMIYOO_BUTTON_SELECT;
    out->guide.kind = EMappingKind_Button;
    out->guide.target = MMIYOO_BUTTON_MENU;
    out->start.kind = EMappingKind_Button;
    out->start.target = MMIYOO_BUTTON_START;
    out->leftshoulder.kind = EMappingKind_Button;
    out->leftshoulder.target = MMIYOO_BUTTON_L1;
    out->rightshoulder.kind = EMappingKind_Button;
    out->rightshoulder.target = MMIYOO_BUTTON_R1;
    out->lefttrigger.kind = EMappingKind_Button;
    out->lefttrigger.target = MMIYOO_BUTTON_L2;
    out->righttrigger.kind = EMappingKind_Button;
    out->righttrigger.target = MMIYOO_BUTTON_R2;
    out->dpup.kind = EMappingKind_Button;
    out->dpup.target = MMIYOO_BUTTON_UP;
    out->dpdown.kind = EMappingKind_Button;
    out->dpdown.target = MMIYOO_BUTTON_DOWN;
    out->dpleft.kind = EMappingKind_Button;
    out->dpleft.target = MMIYOO_BUTTON_LEFT;
    out->dpright.kind = EMappingKind_Button;
    out->dpright.target = MMIYOO_BUTTON_RIGHT;
    out->leftx.kind = EMappingKind_Axis;
    out->leftx.target = 0;
    out->lefty.kind = EMappingKind_Axis;
    out->lefty.target = 1;

    return SDL_TRUE;
}

SDL_JoystickDriver SDL_MMIYOO_JoystickDriver = {
    MMIYOO_JoystickInit,
    MMIYOO_JoystickGetCount,
    MMIYOO_JoystickDetect,
    MMIYOO_JoystickGetDeviceName,
    MMIYOO_JoystickGetDevicePlayerIndex,
    MMIYOO_JoystickSetDevicePlayerIndex,
    MMIYOO_JoystickGetDeviceGUID,
    MMIYOO_JoystickGetDeviceInstanceID,
    MMIYOO_JoystickOpen,
    MMIYOO_JoystickRumble,
    MMIYOO_JoystickRumbleTriggers,
    MMIYOO_JoystickGetCapabilities,
    MMIYOO_JoystickSetLED,
    MMIYOO_JoystickSendEffect,
    MMIYOO_JoystickSetSensorsEnabled,
    MMIYOO_JoystickUpdate,
    MMIYOO_JoystickClose,
    MMIYOO_JoystickQuit,
    MMIYOO_JoystickGetGamepadMapping
};

#endif
