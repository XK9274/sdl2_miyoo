/*
  New translation unit created for the Miyoo-Mini SDL renderer/video split.

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
#ifndef SDL_video_mmiyoo_internal_h_
#define SDL_video_mmiyoo_internal_h_

/* Private cross-file contract for SDL_video_mmiyoo.c, _gfx.c, and
 * _present.c. The renderer and GLES backend see only SDL_video_mmiyoo.h. */

#ifdef MMIYOO
    #include "mi_sys.h"
    #include "mi_gfx.h"
#endif

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
     * gfx.back directly -- its memory byte order is GL's RGBA8888, which
     * MI_GFX calls ABGR8888, not the ARGB8888 our own SW blits use. Unused
     * in pbuffer mode (the default). */
    SDL_bool back_buffer_is_gles_rgba;
} GFX;

extern GFX gfx;

/* Framebuffer metrics -- written by MMIYOO_UpdateFramebufferMetrics,
 * read by present.c's GFX_GetFrame* accessors and video.c's window setup. */
extern int g_framebuffer_width;
extern int g_framebuffer_height;
extern int g_framebuffer_stride;
extern int g_framebuffer_bytes_per_pixel;
extern int g_framebuffer_size;
extern int g_tmp_buffer_size;
extern Uint32 g_framebuffer_format;

/* --- video.c internal API --- */
void MMIYOO_UpdateFramebufferMetrics(void);

/* gfx.c internal API. GFX_Copy, GFX_FlushTextureFences, and
 * GFX_AddTextureFence are declared in the public header instead, since the
 * renderer calls them too. */
int FB_Init(void);
int FB_Uninit(void);
void GFX_Init(void);
void GFX_Quit(void);
void FB_Clear(void);

#endif /* SDL_video_mmiyoo_internal_h_ */
