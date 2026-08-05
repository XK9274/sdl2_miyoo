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

#ifndef SDL_mmiyoo_h_
#define SDL_mmiyoo_h_

typedef enum {
    MMIYOO_MODEL_UNKNOWN = 0,
    MMIYOO_MODEL_MINI = 283,
    MMIYOO_MODEL_PLUS = 354
} MMIYOO_DeviceModel;

extern int MMIYOO_WriteSysfs(const char *path, const char *value, size_t length);
extern SDL_bool MMIYOO_ReadIntFile(const char *path, int *value);

extern MMIYOO_DeviceModel MMIYOO_GetDeviceModel(void);

extern SDL_bool MMIYOO_HasRumble(void);
extern int MMIYOO_SetRumble(SDL_bool enabled);

extern SDL_bool MMIYOO_GetBatteryPercent(int *percent);
extern SDL_bool MMIYOO_IsCharging(SDL_bool *charging);

#endif /* SDL_mmiyoo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
