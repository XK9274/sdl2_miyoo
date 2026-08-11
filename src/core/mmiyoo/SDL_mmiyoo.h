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

typedef enum {
    MMIYOO_BUTTON_UP = 0,
    MMIYOO_BUTTON_DOWN,
    MMIYOO_BUTTON_LEFT,
    MMIYOO_BUTTON_RIGHT,
    MMIYOO_BUTTON_A,
    MMIYOO_BUTTON_B,
    MMIYOO_BUTTON_X,
    MMIYOO_BUTTON_Y,
    MMIYOO_BUTTON_L1,
    MMIYOO_BUTTON_R1,
    MMIYOO_BUTTON_L2,
    MMIYOO_BUTTON_R2,
    MMIYOO_BUTTON_SELECT,
    MMIYOO_BUTTON_START,
    MMIYOO_BUTTON_MENU,
    MMIYOO_BUTTON_QSAVE,
    MMIYOO_BUTTON_QLOAD,
    MMIYOO_BUTTON_FF,
    MMIYOO_BUTTON_EXIT,
    MMIYOO_BUTTON_POWER,
    MMIYOO_BUTTON_VOLUP,
    MMIYOO_BUTTON_VOLDOWN
} MMIYOO_Button;

#define MMIYOO_BUTTON_LAST MMIYOO_BUTTON_VOLDOWN
#define MMIYOO_BUTTON_COUNT (MMIYOO_BUTTON_LAST + 1)

#define MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH  640
#define MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT 480
#define MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL 4

typedef struct {
    int width;
    int height;
    int stride;
    int bytes_per_pixel;
    int active_size;
    Uint32 sdl_format;
} MMIYOO_FramebufferInfo;

extern int MMIYOO_WriteSysfs(const char *path, const char *value, size_t length);
extern SDL_bool MMIYOO_ReadIntFile(const char *path, int *value);

extern MMIYOO_DeviceModel MMIYOO_GetDeviceModel(void);
extern Uint32 MMIYOO_KeycodeToButtonMask(int code);

extern void MMIYOO_GetDefaultFramebufferInfo(MMIYOO_FramebufferInfo *info);
extern SDL_bool MMIYOO_GetFramebufferInfoFromFD(int fb_fd, MMIYOO_FramebufferInfo *info);

extern SDL_bool MMIYOO_HasRumble(void);
extern int MMIYOO_SetRumble(SDL_bool enabled);

extern SDL_bool MMIYOO_GetBatteryPercent(int *percent);
extern SDL_bool MMIYOO_IsCharging(SDL_bool *charging);

/* Shared raw-input layer (single reader of /dev/input/event0), and the live,
 * app-switchable mode deciding whether the video backend's keyboard/mouse
 * emulation or the joystick backend is the one actually posting SDL events
 * from it. Unset hint defaults to joystick. */
#define SDL_HINT_MMIYOO_INPUT_MODE "SDL_MMIYOO_INPUT_MODE"
#define MMIYOO_INPUT_MODE_KEYBOARD "keyboard"
#define MMIYOO_INPUT_MODE_JOYSTICK "joystick"

extern void MMIYOO_InputInit(void);
extern void MMIYOO_InputDeinit(void);
extern Uint32 MMIYOO_GetKeypadBitmap(void);
extern SDL_bool MMIYOO_IsKeyboardModeActive(void);
extern SDL_bool MMIYOO_IsJoystickModeActive(void);

/* Present pacing. Unset/anything unrecognized = off. "strict" is read
 * once at FB_Init (locks in the /dev/l panning buffer layout); off/adaptive
 * are read live every present.
 *
 * Default is "off" for now, pending a decision between adaptive/strict. */
#define SDL_HINT_MMIYOO_VSYNC_MODE "SDL_MMIYOO_VSYNC_MODE"

typedef enum {
    MMIYOO_VSYNC_MODE_OFF, /* default, and any unrecognized/unset value */
    MMIYOO_VSYNC_MODE_ADAPTIVE,
    MMIYOO_VSYNC_MODE_STRICT   /* real /dev/l panning; FB_Init-time only */
} MMIYOO_VSyncMode_e;

extern MMIYOO_VSyncMode_e MMIYOO_GetVSyncMode(void);

#endif /* SDL_mmiyoo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
