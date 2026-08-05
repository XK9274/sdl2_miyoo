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

#ifndef __SDL_EVENT_MMIYOO_H__
#define __SDL_EVENT_MMIYOO_H__

#include "../../SDL_internal.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"

#define MYKEY_UP            MMIYOO_BUTTON_UP
#define MYKEY_DOWN          MMIYOO_BUTTON_DOWN
#define MYKEY_LEFT          MMIYOO_BUTTON_LEFT
#define MYKEY_RIGHT         MMIYOO_BUTTON_RIGHT
#define MYKEY_A             MMIYOO_BUTTON_A
#define MYKEY_B             MMIYOO_BUTTON_B
#define MYKEY_X             MMIYOO_BUTTON_X
#define MYKEY_Y             MMIYOO_BUTTON_Y
#define MYKEY_L1            MMIYOO_BUTTON_L1
#define MYKEY_R1            MMIYOO_BUTTON_R1
#define MYKEY_L2            MMIYOO_BUTTON_L2
#define MYKEY_R2            MMIYOO_BUTTON_R2
#define MYKEY_SELECT        MMIYOO_BUTTON_SELECT
#define MYKEY_START         MMIYOO_BUTTON_START
#define MYKEY_MENU          MMIYOO_BUTTON_MENU
#define MYKEY_QSAVE         MMIYOO_BUTTON_QSAVE
#define MYKEY_QLOAD         MMIYOO_BUTTON_QLOAD
#define MYKEY_FF            MMIYOO_BUTTON_FF
#define MYKEY_EXIT          MMIYOO_BUTTON_EXIT
#define MYKEY_POWER         MMIYOO_BUTTON_POWER
#define MYKEY_VOLUP         MMIYOO_BUTTON_VOLUP
#define MYKEY_VOLDOWN       MMIYOO_BUTTON_VOLDOWN

#define MYKEY_LAST_BITS     18 // ignore POWER, VOL-, VOL+ keys

#define MMIYOO_KEYPAD_MODE 0
#define MMIYOO_MOUSE_MODE  1

typedef struct _MMIYOO_EventInfo {
    struct _keypad{
        uint32_t bitmaps;
    } keypad;

    struct _mouse{
        int x;
        int y;
        int minx;
        int miny;
        int maxx;
        int maxy;
    } mouse;

    int mode;
} MMIYOO_EventInfo;

extern void MMIYOO_EventInit(void);
extern void MMIYOO_EventDeinit(void);
extern void MMIYOO_PumpEvents(_THIS);
extern void MMIYOO_SetMouseBounds(int minx, int miny, int maxx, int maxy);

#endif
