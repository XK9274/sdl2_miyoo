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

#ifndef __SDL_VIDEO_MMIYOO_H__
#define __SDL_VIDEO_MMIYOO_H__

#include <stdint.h>
#include <stdbool.h>
#include <linux/fb.h>

#include "../SDL_sysvideo.h"
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
#include "SDL_video_mmiyoo.h"
#include "SDL_event_mmiyoo.h"
#include "SDL_framebuffer_mmiyoo.h"
#if SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2
#include "SDL_opengles_mmiyoo.h"
#endif

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

typedef struct _GFX {
    int fb_dev;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    struct _DMA {
        void *virAddr;
        MI_PHY phyAddr;
        MI_U32 length;

    } fb, back, overlay;

    struct _HW {
        struct _BUF {
            MI_GFX_Surface_t surf;
            MI_GFX_Rect_t rt;
        } src, dst, overlay;
        MI_GFX_Opt_t opt;
    } hw;

    SDL_bool double_buffer_enabled;

    /* Present strategy: real FBIOPAN_DISPLAY flip vs the fenced BitBlit copy. */
    SDL_bool page_flip_enabled;
    int page_flip_index;

    /* SDL_TRUE once a GLES windowsurface-mode context has written into
     * gfx.back directly (see SDL_opengles_mmiyoo.c) -- its memory byte order
     * is GL's RGBA8888, which MI_GFX calls ABGR8888, not the ARGB8888 our
     * own SW blits use. Unused in pbuffer mode (the default). */
    SDL_bool back_buffer_is_gles_rgba;
} GFX;

void GFX_SetBackBufferGLESFormat(SDL_bool is_gles_rgba);
void FB_Clear(void);
void GFX_FlushTextureFences(void);
void GFX_AddTextureFence(MI_U16 fence);
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

int FB_Init(void);
int FB_Uninit(void);

int My_QueueCopy(SDL_Renderer *renderer,
                 SDL_Texture *texture,
                 const void *pixels,
                 const SDL_Rect *srcrect,
                 const SDL_FRect *dstrect,
                 SDL_BlendMode blend_mode,
                 MI_GFX_Rotate_e extra_rotation,
                 SDL_RendererFlip flip,
                 SDL_FPoint rotation_center,
                 Uint8 mod_r,
                 Uint8 mod_g,
                 Uint8 mod_b,
                 Uint8 mod_a);

#ifndef MMIYOO_LOG_DEBUG
#define MMIYOO_LOG_PREFIX "[MMIYOO] "
#define MMIYOO_LOG_DEBUG(fmt, ...) SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#define MMIYOO_LOG_WARN(fmt, ...)  SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,  MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#define MMIYOO_LOG_ERROR(fmt, ...) SDL_LogError(SDL_LOG_CATEGORY_RENDER, MMIYOO_LOG_PREFIX fmt, ##__VA_ARGS__)
#endif

#endif
