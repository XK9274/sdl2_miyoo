/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

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

#ifndef SDL_POWER_DISABLED
#if SDL_POWER_MMIYOO

#include "SDL_power.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "../SDL_syspower.h"

SDL_bool
SDL_GetPowerInfo_MMIYOO(SDL_PowerState *state, int *seconds, int *percent)
{
    SDL_bool charging = SDL_FALSE;
    SDL_bool have_charging;
    SDL_bool have_percent;
    int battery_percent = -1;

    have_charging = MMIYOO_IsCharging(&charging);
    have_percent = MMIYOO_GetBatteryPercent(&battery_percent);

    *seconds = -1;
    *percent = -1;
    *state = SDL_POWERSTATE_UNKNOWN;

    if (!have_charging && !have_percent) {
        return SDL_FALSE;
    }

    if (have_percent) {
        if (battery_percent >= 0 && battery_percent <= 100) {
            *percent = battery_percent;
        } else if (battery_percent == 500) {
            charging = SDL_TRUE;
            have_charging = SDL_TRUE;
        }
    }

    if (have_charging) {
        if (charging) {
            *state = (*percent == 100) ? SDL_POWERSTATE_CHARGED : SDL_POWERSTATE_CHARGING;
        } else {
            *state = SDL_POWERSTATE_ON_BATTERY;
        }
    } else {
        *state = SDL_POWERSTATE_UNKNOWN;
    }

    return SDL_TRUE;
}

#endif /* SDL_POWER_MMIYOO */
#endif /* SDL_POWER_DISABLED */
