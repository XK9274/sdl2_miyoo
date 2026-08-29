/*
  Customized version for Miyoo-Mini handheld.

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

#include <time.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include "SDL_thread.h"
#include "SDL_mutex.h"
#include "SDL_log.h"
#include "SDL_atomic.h"
#include "SDL_rect.h"
#include "SDL_timer.h"
#include <sys/mman.h>
#include "neon.h"
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <execinfo.h>

#include "../../events/SDL_events_c.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"

#include "SDL_version.h"
#include "SDL_syswm.h"
#include "SDL_loadso.h"
#include "SDL_events.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_video_mmiyoo.h"
#include "SDL_event_mmiyoo.h"
#if SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2
#include "SDL_opengles_mmiyoo.h"
#endif
#include "SDL_framebuffer_mmiyoo.h"
#include "SDL_video_mmiyoo_internal.h"

/* Device availability/creation, window creation/destruction, display-mode
 * setup, video init/quit, and ownership of the public MMiyooVideoInfo
 * object. Delegates to _gfx.c and _present.c for everything else. */

GFX gfx = {0};
MMIYOO_VideoInfo MMiyooVideoInfo = {0};

/* Forward declarations: MMIYOO_CreateDevice wires these into the device
 * struct before their definitions appear further down this file. */
static int MMIYOO_VideoInit(_THIS);
static int MMIYOO_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static void MMIYOO_VideoQuit(_THIS);

static int MMIYOO_Available(void)
{
    const char *envr = SDL_getenv("SDL_VIDEODRIVER");
    if((envr) && (SDL_strcmp(envr, MMIYOO_DRIVER_NAME) == 0)) {
        return 1;
    }

    /* Auto-detect path (SDL_VIDEODRIVER unset); see MMIYOO_ProbeHardware()
     * in core/mmiyoo/SDL_mmiyoo.c. */
    if (MMIYOO_ProbeHardware()) {
        return 1;
    }

    return 0;
}

static void MMIYOO_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->gl_data);
    SDL_free(device);
}

void MMIYOO_DestroyWindow(_THIS, SDL_Window *window)
{
    MMIYOO_LOG_DEBUG("DestroyWindow: window id=%u ptr=%p", window->id, (void*)window);
}

/* Restores mouse/keyboard focus and event routing to `window`. Exposed via
 * the public SDL_RaiseWindow() -- needed by callers that create a second,
 * throwaway window (e.g. an offscreen GL context for a loading-screen
 * effect) after the real window already had focus, since MMIYOO_CreateWindow
 * unconditionally grabs focus for every window it creates. */

void MMIYOO_RaiseWindow(_THIS, SDL_Window *window)
{
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);
    MMiyooVideoInfo.window = window;
}

int MMIYOO_CreateWindow(_THIS, SDL_Window *window)
{
    int target_w = window->w;
    int target_h = window->h;

    if (target_w <= 0) {
        if (window->fullscreen_mode.w > 0) {
            target_w = window->fullscreen_mode.w;
        } else if (g_framebuffer_width > 0) {
            target_w = g_framebuffer_width;
        } else {
            target_w = MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
        }
    }

    if (target_h <= 0) {
        if (window->fullscreen_mode.h > 0) {
            target_h = window->fullscreen_mode.h;
        } else if (g_framebuffer_height > 0) {
            target_h = g_framebuffer_height;
        } else {
            target_h = MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
        }
    }

    window->w = target_w;
    window->h = target_h;
    window->windowed.w = target_w;
    window->windowed.h = target_h;

    MMIYOO_LOG_DEBUG("CreateWindow: requested=%dx%d final=%dx%d flags=0x%x id=%u ptr=%p",
                     window->w, window->h, target_w, target_h,
                     (unsigned int)window->flags,
                     (unsigned int)window->id, (void*)window);

    SDL_OnWindowResized(window);
    SDL_SetMouseFocus(window);
    /* One window, it always has focus -- without this, SDL_PrivateJoystickButton drops every joystick press as unfocused. window->flags always has SDL_WINDOW_HIDDEN set here (SDL_video.c ORs it in unconditionally), so this can't be gated on HIDDEN to skip a throwaway offscreen-GL window; such callers must restore focus themselves via MMIYOO_RaiseWindow. */
    SDL_SetKeyboardFocus(window);
    MMiyooVideoInfo.window = window;
    return 0;
}

int MMIYOO_CreateWindowFrom(_THIS, SDL_Window *window, const void *data)
{
    return SDL_Unsupported();
}

static SDL_VideoDevice *MMIYOO_CreateDevice(int devindex)
{
    SDL_VideoDevice *device=NULL;
#if SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2
    SDL_GLDriverData *gldata=NULL;
#endif

    if(!MMIYOO_Available()) {
        return (0);
    }

    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if(!device) {
        SDL_OutOfMemory();
        return (0);
    }
    device->is_dummy = SDL_FALSE;

    device->VideoInit = MMIYOO_VideoInit;
    device->VideoQuit = MMIYOO_VideoQuit;
    device->SetDisplayMode = MMIYOO_SetDisplayMode;
    device->PumpEvents = MMIYOO_PumpEvents;
    device->CreateSDLWindow = MMIYOO_CreateWindow;
    device->CreateSDLWindowFrom = MMIYOO_CreateWindowFrom;
    device->CreateWindowFramebuffer = MMIYOO_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = MMIYOO_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = MMIYOO_DestroyWindowFramebuffer;
    device->DestroyWindow = MMIYOO_DestroyWindow;
    device->RaiseWindow = MMIYOO_RaiseWindow;

#if SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2
    device->GL_LoadLibrary = glLoadLibrary;
    device->GL_GetProcAddress = glGetProcAddress;
    device->GL_CreateContext = glCreateContext;
    device->GL_SetSwapInterval = glSetSwapInterval;
    device->GL_GetSwapInterval = glGetSwapInterval;
    device->GL_SwapWindow = glSwapWindow;
    device->GL_MakeCurrent = glMakeCurrent;
    device->GL_DeleteContext = glDeleteContext;
    device->GL_UnloadLibrary = glUnloadLibrary;
    device->GL_DefaultProfileConfig = MMIYOO_GLES_DefaultProfileConfig;

    gldata = (SDL_GLDriverData*)SDL_calloc(1, sizeof(SDL_GLDriverData));
    if(gldata == NULL) {
        SDL_OutOfMemory();
        SDL_free(device);
        return NULL;
    }

    gldata->display = EGL_NO_DISPLAY;
    gldata->context = EGL_NO_CONTEXT;
    gldata->surface = EGL_NO_SURFACE;
    gldata->config = NULL;
    gldata->swap_interval = 1;

    device->gl_data = gldata;
#endif
    device->free = MMIYOO_DeleteDevice;
    return device;
}

VideoBootStrap MMIYOO_bootstrap = {MMIYOO_DRIVER_NAME, "MMIYOO VIDEO DRIVER", MMIYOO_CreateDevice};

int MMIYOO_VideoInit(_THIS)
{
    SDL_DisplayMode native_mode;
    SDL_DisplayMode alt_mode;
    int native_w;
    int native_h;
    Uint32 native_format;
    int display_index;
    SDL_VideoDisplay *display = NULL;

    SDL_zero(native_mode);
    SDL_zero(alt_mode);

    GFX_Init();
    MMIYOO_UpdateFramebufferMetrics();

    native_w = (g_framebuffer_width > 0) ? g_framebuffer_width : MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
    native_h = (g_framebuffer_height > 0) ? g_framebuffer_height : MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
    native_format = g_framebuffer_format;

    native_mode.format = native_format;
    native_mode.w = native_w;
    native_mode.h = native_h;
    native_mode.refresh_rate = 60;
    native_mode.driverdata = NULL;

    MMIYOO_LOG_WARN("VideoInit: native_w=%d native_h=%d format=0x%x", native_w, native_h, native_format);

    display_index = SDL_AddBasicVideoDisplay(&native_mode);
    if (display_index < 0) {
        MMIYOO_LOG_ERROR("VideoInit: SDL_AddBasicVideoDisplay failed: %s", SDL_GetError());
        return -1;
    }

    display = SDL_GetDisplay(display_index);
    if (display == NULL) {
        MMIYOO_LOG_ERROR("VideoInit: SDL_GetDisplay failed after AddBasicVideoDisplay");
        return -1;
    }

    alt_mode = native_mode;
    alt_mode.format = (native_format == SDL_PIXELFORMAT_RGB565) ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;
    SDL_AddDisplayMode(display, &alt_mode);

    MMIYOO_LOG_WARN("VideoInit: _this->displays[%d].current_mode = %dx%d",
                    display_index,
                    display->current_mode.w,
                    display->current_mode.h);

    MMIYOO_EventInit();
    return 0;
}

static int MMIYOO_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return 0;
}

void MMIYOO_VideoQuit(_THIS)
{
    MMIYOO_EventDeinit();
    GFX_Quit();
}

#endif /* SDL_VIDEO_DRIVER_MMIYOO */
