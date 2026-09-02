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

#ifndef __SDL_VIDEO_MMIYOO_H__
#define __SDL_VIDEO_MMIYOO_H__

#include <stdint.h>
#include <stdbool.h>
#include <linux/fb.h>

#include "../SDL_sysvideo.h"
#include "SDL_log.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_render.h"
#include "SDL_version.h"
#include "SDL_syswm.h"
#include "SDL_loadso.h"
#include "SDL_events.h"
#include "SDL_video.h"
#include "SDL_thread.h"
#include "SDL_mutex.h"
#include "SDL_mouse.h"

#ifdef MMIYOO
    #include "mi_sys.h"
    #include "mi_gfx.h"
#endif

#ifndef MAX_PATH
    #define MAX_PATH 128
#endif

#define PREFIX                      "[SDL] "
#define MMIYOO_DRIVER_NAME          "mmiyoo"

typedef struct MMIYOO_VideoInfo {
    SDL_Window *window;
} MMIYOO_VideoInfo;

/* GFX/window internal state (the _GFX/GFX struct, framebuffer metrics, and
 * FB_Init/FB_Uninit/GFX_Init/GFX_Quit/FB_Clear) is private to video.c/_gfx.c/
 * _present.c -- see SDL_video_mmiyoo_internal.h. This header carries only the
 * GFX contract functions the renderer and GLES backend actually call. */

void GFX_SetBackBufferGLESFormat(SDL_bool is_gles_rgba);
void GFX_FlushTextureFences(void);
void GFX_AddTextureFence(MI_U16 fence);
SDL_bool GFX_HasPendingTextureFences(void);
int GFX_Copy(const void *pixels,
             MI_PHY pixels_phy,
             SDL_Rect srcrect,
             SDL_Rect dstrect,
             int pitch,
             int rotate,
             MI_GFX_Mirror_e mirror,
             SDL_BlendMode blend_mode,
             MI_GFX_Surface_t *target_surface,
             const SDL_Rect *clip_rect,
             SDL_bool clip_enabled,
             Uint32 src_format,
             MI_GFX_ColorFmt_e src_mi_format,
             Uint32 bytes_per_pixel,
             Uint8 mod_r,
             Uint8 mod_g,
             Uint8 mod_b,
             Uint8 mod_a);

// Single/double buffer management
MI_PHY GFX_GetFrameBuffer(void);
MI_U32 GFX_GetFrameStride(void);
MI_U32 GFX_GetFrameWidth(void);
MI_U32 GFX_GetFrameHeight(void);
SDL_bool GFX_IsPageFlipEnabled(void);
void GFX_SwapBuffers(SDL_bool wait_for_vsync);
void *GFX_GetFrameBufferVirtual(void);
void *GFX_GetOverlayVirtual(void);
MI_PHY GFX_GetOverlayPhysical(void);

#ifndef MMIYOO_LOG_DEBUG
#define MMIYOO_LOG_PREFIX "[MMIYOO] "
#define MMIYOO_LOG_DEBUG(fmt, ...) SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#define MMIYOO_LOG_WARN(fmt, ...)  SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,  MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#define MMIYOO_LOG_ERROR(fmt, ...) SDL_LogError(SDL_LOG_CATEGORY_RENDER, MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#endif

#endif
