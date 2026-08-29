/*
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
#include "SDL_video_mmiyoo_internal.h"

/* Framebuffer metrics, system/GFX init+teardown, framebuffer init/uninit,
 * shared MI_GFX copy/blit configuration+execution (used by the renderer and
 * GLES), MI_SYS/GFX reference counts, crash-handler setup, and
 * framebuffer/back-buffer/overlay allocation/mapping with failure unwinding. */

int g_framebuffer_width = 0;
int g_framebuffer_height = 0;
int g_framebuffer_stride = 0;
int g_framebuffer_bytes_per_pixel = 4;
int g_framebuffer_size = 0;
int g_tmp_buffer_size = 0;
Uint32 g_framebuffer_format = SDL_PIXELFORMAT_ARGB8888;

void
MMIYOO_UpdateFramebufferMetrics(void)
{
    MMIYOO_FramebufferInfo info;

    if (!MMIYOO_GetFramebufferInfoFromFD(gfx.fb_dev, &info)) {
        MMIYOO_GetDefaultFramebufferInfo(&info);
    }

    g_framebuffer_width = info.width;
    g_framebuffer_height = info.height;
    g_framebuffer_bytes_per_pixel = info.bytes_per_pixel;
    g_framebuffer_stride = info.stride;
    g_framebuffer_size = info.active_size;
    g_framebuffer_format = info.sdl_format;
    g_tmp_buffer_size = g_framebuffer_size;
}

// Global texture fence management for performance
#define MAX_GLOBAL_FENCES 512
static MI_U16 global_texture_fences[MAX_GLOBAL_FENCES];
static int global_fence_count = 0;

static SDL_Surface *cvt = NULL;
static SDL_SpinLock g_mmiyoo_sys_lock = 0;
static int g_mmiyoo_sys_refcount = 0;
static int g_mmiyoo_gfx_refcount = 0;

/* Real-linkage symbols compiled into SDL_render.c, always in the same
 * libSDL2-2.0.so.0. Declared extern here (as SDL_sysrender.h itself does)
 * so this src/video/ file can decompose composed blend modes without
 * pulling in the src/render/-internal header. */
extern SDL_BlendFactor SDL_GetBlendModeSrcColorFactor(SDL_BlendMode blendMode);
extern SDL_BlendFactor SDL_GetBlendModeDstColorFactor(SDL_BlendMode blendMode);
extern SDL_BlendOperation SDL_GetBlendModeColorOperation(SDL_BlendMode blendMode);
extern SDL_BlendFactor SDL_GetBlendModeSrcAlphaFactor(SDL_BlendMode blendMode);
extern SDL_BlendFactor SDL_GetBlendModeDstAlphaFactor(SDL_BlendMode blendMode);
extern SDL_BlendOperation SDL_GetBlendModeAlphaOperation(SDL_BlendMode blendMode);

/* Latched once per process: whether a composed blend mode is representable
 * on this hardware is a static fact about that mode, not renderer state. */
static SDL_bool g_warned_unsupported_compose = SDL_FALSE;

static void
MMIYOO_WaitGFXIdle(void)
{
    MI_GFX_WaitAllDone(TRUE, 0);
}

// Waits on every queued fence individually rather than just the highest one:
// MI_GFX doesn't document that QuickFill/BitBlit/DrawLine fences complete in
// strict issue order, and assuming so caused visible top-of-screen tearing
// (an earlier fill could still be in flight when a later fence was reached).

void GFX_FlushTextureFences(void)
{
#ifdef MMIYOO
    int i;
    for (i = 0; i < global_fence_count; i++) {
        MI_GFX_WaitAllDone(FALSE, global_texture_fences[i]);
    }
    global_fence_count = 0;
#endif
}

// Single tracking point for all queued GFX fences (BitBlit, QuickFill, DrawLine)

void GFX_AddTextureFence(MI_U16 fence)
{
#ifdef MMIYOO
    if (global_fence_count >= MAX_GLOBAL_FENCES) {
        GFX_FlushTextureFences();
    }
    global_texture_fences[global_fence_count++] = fence;
#endif
}

static void MMIYOO_CrashHandler(int sig) {
    void *array[10];
    size_t size;
    char **strings;
    size_t i;

    size = backtrace(array, 10);
    strings = backtrace_symbols(array, size);

    if (strings != NULL) {
        /* Async-signal-safe: raw write(), not SDL_Log/fprintf. */
        for (i = 0; i < size; i++) {
            write(STDERR_FILENO, strings[i], strlen(strings[i]));
            write(STDERR_FILENO, "\n", 1);
        }
        free(strings);
    }

    // Reset signal to default and re-raise to get core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef MMIYOO

int FB_Init(void)
{
    MI_U32 frame_stride;
    MI_U32 frame_bytes;

    SDL_AtomicLock(&g_mmiyoo_sys_lock);
    if (g_mmiyoo_sys_refcount == 0) {
        MI_S32 ret = MI_SYS_Init();
        if (ret != MI_SUCCESS) {
            SDL_AtomicUnlock(&g_mmiyoo_sys_lock);
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                         "FB_Init: MI_SYS_Init failed (0x%x)", ret);
            return -1;
        }
    }
    g_mmiyoo_sys_refcount++;

    if (g_mmiyoo_gfx_refcount == 0) {
        MI_S32 ret = MI_GFX_Open();
        if (ret != MI_SUCCESS) {
            --g_mmiyoo_sys_refcount;
            if (g_mmiyoo_sys_refcount == 0) {
                MI_SYS_Exit();
            }
            SDL_AtomicUnlock(&g_mmiyoo_sys_lock);
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                         "FB_Init: MI_GFX_Open failed (0x%x)", ret);
            return -1;
        }
    }
    g_mmiyoo_gfx_refcount++;
    SDL_AtomicUnlock(&g_mmiyoo_sys_lock);

    gfx.double_buffer_enabled = SDL_TRUE;
    gfx.page_flip_enabled = SDL_FALSE;
    gfx.page_flip_index = 0;

    gfx.fb_dev = open("/dev/fb0", O_RDWR);
    ioctl(gfx.fb_dev, FBIOGET_FSCREENINFO, &gfx.finfo);
    ioctl(gfx.fb_dev, FBIOGET_VSCREENINFO, &gfx.vinfo);
    gfx.vinfo.yoffset = 0;

    if (MMIYOO_GetVSyncMode() == MMIYOO_VSYNC_MODE_STRICT) {
        /* Try real panning double buffering: two pages in one fb0 mapping. */
        struct fb_var_screeninfo verify;

        gfx.vinfo.yres_virtual = gfx.vinfo.yres * 2;
        ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);
        ioctl(gfx.fb_dev, FBIOGET_VSCREENINFO, &verify);

        if (verify.yres_virtual >= gfx.vinfo.yres * 2) {
            gfx.vinfo = verify;
            ioctl(gfx.fb_dev, FBIOGET_FSCREENINFO, &gfx.finfo);
            gfx.page_flip_enabled = SDL_TRUE;
            MMIYOO_LOG_DEBUG("FB_Init: /dev/l vsync active (panel honours FBIOPAN_DISPLAY)");
        } else {
            gfx.vinfo.yres_virtual = gfx.vinfo.yres;
            ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);
            MMIYOO_LOG_WARN("FB_Init: /dev/l vsync requested but panel rejected panning, falling back to present-copy");
        }
    } else {
        gfx.vinfo.yres_virtual = gfx.vinfo.yres;
        ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);
    }

    MMIYOO_UpdateFramebufferMetrics();

    frame_stride = (Uint32)g_framebuffer_stride;
    frame_bytes = (Uint32)g_framebuffer_size;
    if (frame_stride == 0 || frame_bytes == 0) {
        Uint32 fallback_bpp = (Uint32)g_framebuffer_bytes_per_pixel;
        if (fallback_bpp == 0) {
            fallback_bpp = 4;
        }
        frame_stride = (Uint32)g_framebuffer_width * fallback_bpp;
        frame_bytes = frame_stride * (Uint32)g_framebuffer_height;
    }

    gfx.fb.phyAddr = gfx.finfo.smem_start;
    if (gfx.page_flip_enabled) {
        gfx.fb.length = frame_bytes * 2;
    } else {
        gfx.fb.length = (gfx.finfo.smem_len > 0) ? gfx.finfo.smem_len : frame_bytes;
        if (gfx.fb.length == 0) {
            gfx.fb.length = frame_bytes;
        }
    }
    g_framebuffer_size = frame_bytes;
    g_tmp_buffer_size = frame_bytes;

    if (gfx.fb.phyAddr != 0 && gfx.fb.length > 0) {
        MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, gfx.fb.length);
        MI_SYS_Mmap(gfx.fb.phyAddr, gfx.fb.length, &gfx.fb.virAddr, TRUE);
    }

    memset(&gfx.hw.opt, 0, sizeof(gfx.hw.opt));

    if (MI_SYS_MMA_Alloc(NULL, g_tmp_buffer_size, &gfx.overlay.phyAddr) == MI_SUCCESS) {
        gfx.overlay.length = g_tmp_buffer_size;
        MI_SYS_Mmap(gfx.overlay.phyAddr, gfx.overlay.length, &gfx.overlay.virAddr, TRUE);
    }

    if (gfx.page_flip_enabled) {
        /* gfx.back is a view into the second page of the single fb0 mapping. */
        gfx.back.phyAddr = gfx.fb.phyAddr + frame_bytes;
        gfx.back.virAddr = (Uint8 *)gfx.fb.virAddr + frame_bytes;
        gfx.back.length = frame_bytes;
    } else if (gfx.double_buffer_enabled) {
        if (MI_SYS_MMA_Alloc(NULL, frame_bytes, &gfx.back.phyAddr) == MI_SUCCESS) {
            gfx.back.length = frame_bytes;
            if (MI_SYS_Mmap(gfx.back.phyAddr, gfx.back.length, &gfx.back.virAddr, TRUE) == MI_SUCCESS) {
                MI_SYS_MemsetPa(gfx.back.phyAddr, 0, gfx.back.length);
            } else {
                MMIYOO_LOG_WARN("FB_Init: failed to map back buffer, disabling double buffer");
                MI_SYS_MMA_Free(gfx.back.phyAddr);
                gfx.back.phyAddr = 0;
                gfx.back.length = 0;
                gfx.back.virAddr = NULL;
                gfx.double_buffer_enabled = SDL_FALSE;
            }
        } else {
            MMIYOO_LOG_WARN("FB_Init: failed to allocate back buffer, disabling double buffer");
            gfx.double_buffer_enabled = SDL_FALSE;
        }
    }

    return 0;
}

int FB_Uninit(void)
{
    MMIYOO_WaitGFXIdle();

    /* Page-flip mode: gfx.back is a view into gfx.fb's mapping, not its own MMA allocation. */
    if (!gfx.page_flip_enabled) {
        if (gfx.back.virAddr && gfx.back.length > 0) {
            MI_SYS_Munmap(gfx.back.virAddr, gfx.back.length);
            gfx.back.virAddr = NULL;
        }
        if (gfx.back.phyAddr) {
            MI_SYS_MMA_Free(gfx.back.phyAddr);
            gfx.back.phyAddr = 0;
            gfx.back.length = 0;
        }
    } else {
        gfx.back.virAddr = NULL;
        gfx.back.phyAddr = 0;
        gfx.back.length = 0;
    }

    if (gfx.fb.virAddr && gfx.fb.length > 0) {
        MI_SYS_Munmap(gfx.fb.virAddr, gfx.fb.length);
        gfx.fb.virAddr = NULL;
    }
    if (gfx.overlay.virAddr && gfx.overlay.length > 0) {
        MI_SYS_Munmap(gfx.overlay.virAddr, gfx.overlay.length);
        gfx.overlay.virAddr = NULL;
    }
    if (gfx.overlay.phyAddr) {
        MI_SYS_MMA_Free(gfx.overlay.phyAddr);
        gfx.overlay.phyAddr = 0;
        gfx.overlay.length = 0;
    }

    gfx.double_buffer_enabled = SDL_FALSE;
    gfx.page_flip_enabled = SDL_FALSE;

    SDL_AtomicLock(&g_mmiyoo_sys_lock);
    if (g_mmiyoo_gfx_refcount > 0) {
        g_mmiyoo_gfx_refcount--;
        if (g_mmiyoo_gfx_refcount == 0) {
            MMIYOO_WaitGFXIdle();
            MI_GFX_Close();
        }
    }

    if (g_mmiyoo_sys_refcount > 0) {
        g_mmiyoo_sys_refcount--;
        if (g_mmiyoo_sys_refcount == 0) {
            MI_S32 ret = MI_SYS_Exit();
            if (ret != MI_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "FB_Uninit: MI_SYS_Exit returned 0x%x", ret);
            }
        }
    }
    SDL_AtomicUnlock(&g_mmiyoo_sys_lock);
    return 0;
}
#endif

void GFX_Init(void)
{
    signal(SIGSEGV, MMIYOO_CrashHandler);
    signal(SIGBUS, MMIYOO_CrashHandler);
    signal(SIGABRT, MMIYOO_CrashHandler);

    if (FB_Init() != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                     "GFX_Init: FB_Init failed");
        return;
    }

    cvt = SDL_CreateRGBSurface(SDL_SWSURFACE,
                               g_framebuffer_width > 0 ? g_framebuffer_width : MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH,
                               g_framebuffer_height > 0 ? g_framebuffer_height : MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT,
                               32, 0, 0, 0, 0);
}

void GFX_Quit(void)
{
    FB_Clear();

    FB_Uninit();

    // Single buffer mode - no yoffset to reset
    close(gfx.fb_dev);
    gfx.fb_dev = 0;

    if (cvt) {
        SDL_FreeSurface(cvt);
        cvt = NULL;
    }
}

void FB_Clear(void)
{
#ifdef MMIYOO
    if (gfx.fb.phyAddr && gfx.fb.length > 0) {
        MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, gfx.fb.length);
    }
    if (gfx.double_buffer_enabled && gfx.back.phyAddr && gfx.back.length > 0) {
        MI_SYS_MemsetPa(gfx.back.phyAddr, 0, gfx.back.length);
    }
#endif
}

/* SDL_BlendFactor -> MI_GFX_DfbBldOp_e. MI_GFX's SRCALPHASAT/MAX have no
 * SDL_BlendFactor equivalent and are never produced here (MAX is only ever
 * returned as this switch's own unreachable-default sentinel). */
static MI_GFX_DfbBldOp_e
MMIYOO_SDLBlendFactorToDfbBldOp(SDL_BlendFactor factor)
{
    switch (factor) {
        case SDL_BLENDFACTOR_ZERO:                return E_MI_GFX_DFB_BLD_ZERO;
        case SDL_BLENDFACTOR_ONE:                 return E_MI_GFX_DFB_BLD_ONE;
        case SDL_BLENDFACTOR_SRC_COLOR:           return E_MI_GFX_DFB_BLD_SRCCOLOR;
        case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR: return E_MI_GFX_DFB_BLD_INVSRCCOLOR;
        case SDL_BLENDFACTOR_SRC_ALPHA:           return E_MI_GFX_DFB_BLD_SRCALPHA;
        case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA: return E_MI_GFX_DFB_BLD_INVSRCALPHA;
        case SDL_BLENDFACTOR_DST_COLOR:           return E_MI_GFX_DFB_BLD_DESTCOLOR;
        case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR: return E_MI_GFX_DFB_BLD_INVDESTCOLOR;
        case SDL_BLENDFACTOR_DST_ALPHA:           return E_MI_GFX_DFB_BLD_DESTALPHA;
        case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA: return E_MI_GFX_DFB_BLD_INVDESTALPHA;
        default:                                  return E_MI_GFX_DFB_BLD_MAX;
    }
}

/* Decomposes a composed SDL_BlendMode and checks whether it fits MI_GFX's
 * single (eSrcDfbBldOp, eDstDfbBldOp) pair applied uniformly across all 4
 * channels with an implicit ADD combine -- the same tier of support SDL's
 * own OpenGL ES1 renderer documents. Returns SDL_FALSE (leaving *src_op/
 * *dst_op untouched) if the mode needs a non-ADD operation or mismatched
 * color-vs-alpha factors. */
static SDL_bool
MMIYOO_TryComposeBlendMode(SDL_BlendMode blend_mode, MI_GFX_DfbBldOp_e *src_op, MI_GFX_DfbBldOp_e *dst_op)
{
    SDL_BlendFactor srcColor = SDL_GetBlendModeSrcColorFactor(blend_mode);
    SDL_BlendFactor dstColor = SDL_GetBlendModeDstColorFactor(blend_mode);
    SDL_BlendOperation colorOp = SDL_GetBlendModeColorOperation(blend_mode);
    SDL_BlendFactor srcAlpha = SDL_GetBlendModeSrcAlphaFactor(blend_mode);
    SDL_BlendFactor dstAlpha = SDL_GetBlendModeDstAlphaFactor(blend_mode);
    SDL_BlendOperation alphaOp = SDL_GetBlendModeAlphaOperation(blend_mode);

    if (colorOp != SDL_BLENDOPERATION_ADD || alphaOp != SDL_BLENDOPERATION_ADD) {
        return SDL_FALSE;
    }
    if (srcColor != srcAlpha || dstColor != dstAlpha) {
        return SDL_FALSE;
    }

    *src_op = MMIYOO_SDLBlendFactorToDfbBldOp(srcColor);
    *dst_op = MMIYOO_SDLBlendFactorToDfbBldOp(dstColor);
    return SDL_TRUE;
}

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
             Uint8 mod_a)
{
#ifdef MMIYOO
    MI_U16 u16Fence = 0;
    MI_S32 result;
    Uint32 src_bytes_per_pixel = bytes_per_pixel;
    MI_GFX_ColorFmt_e mi_src_format = src_mi_format;
    SDL_bool format_supported = SDL_FALSE;

    if (src_bytes_per_pixel != 2 && src_bytes_per_pixel != 4) {
        src_bytes_per_pixel = SDL_BYTESPERPIXEL(src_format);
    }
    if (src_bytes_per_pixel != 2 && src_bytes_per_pixel != 4) {
        src_bytes_per_pixel = 4;
    }

    switch (mi_src_format) {
        case E_MI_GFX_FMT_RGB565:
        case E_MI_GFX_FMT_BGR565:
        case E_MI_GFX_FMT_ARGB8888:
        case E_MI_GFX_FMT_ABGR8888:
        case E_MI_GFX_FMT_BGRA8888:
        case E_MI_GFX_FMT_ARGB1555:
        case E_MI_GFX_FMT_ARGB4444:
        case E_MI_GFX_FMT_RGBA4444:
            format_supported = SDL_TRUE;
            break;
        default:
            break;
    }

    if (!format_supported) {
        switch(src_format) {
            case SDL_PIXELFORMAT_RGB565:
                mi_src_format = E_MI_GFX_FMT_RGB565;
                src_bytes_per_pixel = 2;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_BGR565:
                mi_src_format = E_MI_GFX_FMT_BGR565;
                src_bytes_per_pixel = 2;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_ARGB8888:
                mi_src_format = E_MI_GFX_FMT_ARGB8888;
                src_bytes_per_pixel = 4;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_RGBA8888:
                mi_src_format = E_MI_GFX_FMT_ARGB8888;
                src_bytes_per_pixel = 4;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_ABGR8888:
                mi_src_format = E_MI_GFX_FMT_ABGR8888;
                src_bytes_per_pixel = 4;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_BGRA8888:
                mi_src_format = E_MI_GFX_FMT_BGRA8888;
                src_bytes_per_pixel = 4;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_ARGB1555:
                mi_src_format = E_MI_GFX_FMT_ARGB1555;
                src_bytes_per_pixel = 2;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_ARGB4444:
                mi_src_format = E_MI_GFX_FMT_ARGB4444;
                src_bytes_per_pixel = 2;
                format_supported = SDL_TRUE;
                break;
            case SDL_PIXELFORMAT_RGBA4444:
                mi_src_format = E_MI_GFX_FMT_RGBA4444;
                src_bytes_per_pixel = 2;
                format_supported = SDL_TRUE;
                break;
            default:
                break;
        }
    }

    if (!format_supported) {
        mi_src_format = E_MI_GFX_FMT_ARGB8888;
        src_bytes_per_pixel = 4;
    }

    if (!target_surface) {
        MMIYOO_LOG_WARN("  target_surface is NULL");
    }
    
    if (src_bytes_per_pixel != 2 && src_bytes_per_pixel != 4) {
        MMIYOO_LOG_ERROR("GFX_Copy: unsupported bytes_per_pixel=%u (pitch=%d srcrect.w=%d)",
                         src_bytes_per_pixel, pitch, srcrect.w);
        return -1;
    }
    
#endif

#ifdef MMIYOO
    u16Fence = 0;

    if (pixels == NULL) {
        MMIYOO_LOG_ERROR("GFX_Copy: pixels pointer is NULL");
        return -1;
    }

    gfx.hw.opt.u32GlobalSrcConstColor = 0;
    gfx.hw.opt.u32GlobalDstConstColor = 0;
    gfx.hw.opt.eRotate = rotate;
    gfx.hw.opt.eMirror = mirror;

    /* Disable colorkey operations for predictable blending */
    gfx.hw.opt.stSrcColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */
    gfx.hw.opt.stDstColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */

    /* Blend-factor mapping follows SigmaStar's DfbBldOp_e/DfbBlendFlags_e
     * docs, applied to SDL's composed blend modes. */
    /* Only set ALPHACHANNEL when the source format actually has alpha (RGB565/BGR565 don't). */
    {
        const SDL_bool src_has_alpha = (mi_src_format != E_MI_GFX_FMT_RGB565 &&
                                         mi_src_format != E_MI_GFX_FMT_BGR565);

        switch (blend_mode) {
            case SDL_BLENDMODE_NONE:
                gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
                gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
                gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
                break;
            case SDL_BLENDMODE_ADD:
                gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_SRCALPHA;
                gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
                gfx.hw.opt.eDFBBlendFlag =
                    src_has_alpha ? E_MI_GFX_DFB_BLEND_ALPHACHANNEL : E_MI_GFX_DFB_BLEND_NOFX;
                break;
            case SDL_BLENDMODE_MOD:
                gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
                gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_SRCCOLOR;
                gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
                break;
            case SDL_BLENDMODE_MUL:
                gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_DESTCOLOR;
                gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
                gfx.hw.opt.eDFBBlendFlag =
                    src_has_alpha ? E_MI_GFX_DFB_BLEND_ALPHACHANNEL : E_MI_GFX_DFB_BLEND_NOFX;
                break;
            default: {
                MI_GFX_DfbBldOp_e composed_src_op, composed_dst_op;
                if (MMIYOO_TryComposeBlendMode(blend_mode, &composed_src_op, &composed_dst_op)) {
                    gfx.hw.opt.eSrcDfbBldOp = composed_src_op;
                    gfx.hw.opt.eDstDfbBldOp = composed_dst_op;
                    gfx.hw.opt.eDFBBlendFlag =
                        src_has_alpha ? E_MI_GFX_DFB_BLEND_ALPHACHANNEL : E_MI_GFX_DFB_BLEND_NOFX;
                } else {
                    if (blend_mode != SDL_BLENDMODE_BLEND && !g_warned_unsupported_compose) {
                        MMIYOO_LOG_WARN("GFX_Copy: composed blend mode 0x%x not representable on MI_GFX "
                                         "(needs non-ADD operation or mismatched color/alpha factors), "
                                         "falling back to standard alpha blending",
                                         (unsigned)blend_mode);
                        g_warned_unsupported_compose = SDL_TRUE;
                    }
                    /* SDL_BLENDMODE_BLEND and any unrepresentable composed modes fall back to standard alpha blending */
                    gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_SRCALPHA;
                    gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
                    gfx.hw.opt.eDFBBlendFlag =
                        src_has_alpha ? E_MI_GFX_DFB_BLEND_ALPHACHANNEL : E_MI_GFX_DFB_BLEND_NOFX;
                }
                break;
            }
        }
    }

    /* COLORIZE/COLORALPHA tint the source via u32GlobalSrcConstColor (A8:R8:G8:B8); skip entirely when there's nothing to modulate. */
    if (mod_r != 255 || mod_g != 255 || mod_b != 255 || mod_a != 255) {
        MI_U32 flags = (MI_U32)gfx.hw.opt.eDFBBlendFlag;
        if (mod_r != 255 || mod_g != 255 || mod_b != 255) {
            flags |= (MI_U32)E_MI_GFX_DFB_BLEND_COLORIZE;
        }
        if (mod_a != 255) {
            flags |= (MI_U32)E_MI_GFX_DFB_BLEND_COLORALPHA;
        }
        gfx.hw.opt.eDFBBlendFlag = (MI_Gfx_DfbBlendFlags_e)flags;
        gfx.hw.opt.u32GlobalSrcConstColor = ((MI_U32)mod_a << 24) | ((MI_U32)mod_r << 16) |
                                            ((MI_U32)mod_g << 8) | (MI_U32)mod_b;
    }

    /* Apply clipping if enabled; incoming clip rect is already in target coordinate space */
    if (clip_enabled && clip_rect) {
        gfx.hw.opt.stClipRect.s32Xpos = clip_rect->x;
        gfx.hw.opt.stClipRect.s32Ypos = clip_rect->y;
        gfx.hw.opt.stClipRect.u32Width = clip_rect->w;
        gfx.hw.opt.stClipRect.u32Height = clip_rect->h;
    } else {
        /* No clipping - set to full target area */
        if (target_surface) {
            gfx.hw.opt.stClipRect.s32Xpos = 0;
            gfx.hw.opt.stClipRect.s32Ypos = 0;
            gfx.hw.opt.stClipRect.u32Width = target_surface->u32Width;
            gfx.hw.opt.stClipRect.u32Height = target_surface->u32Height;
        } else {
            gfx.hw.opt.stClipRect.s32Xpos = 0;
            gfx.hw.opt.stClipRect.s32Ypos = 0;
            gfx.hw.opt.stClipRect.u32Width = GFX_GetFrameWidth();
            gfx.hw.opt.stClipRect.u32Height = GFX_GetFrameHeight();
        }
    }
    gfx.hw.src.surf.eColorFmt = mi_src_format;

    {
        MI_PHY offset = (MI_PHY)srcrect.y * pitch + (MI_PHY)srcrect.x * src_bytes_per_pixel;

        gfx.hw.src.rt.s32Xpos = 0;
        gfx.hw.src.rt.s32Ypos = 0;
        gfx.hw.src.rt.u32Width = srcrect.w;
        gfx.hw.src.rt.u32Height = srcrect.h;
        gfx.hw.src.surf.u32Width = srcrect.w;
        gfx.hw.src.surf.u32Height = srcrect.h;
        gfx.hw.src.surf.u32Stride = pitch;
        gfx.hw.src.surf.phyAddr = pixels_phy + offset;
    }

    /* Setup destination rectangle using renderer-space coordinates */
    gfx.hw.dst.rt.s32Xpos = dstrect.x;
    gfx.hw.dst.rt.s32Ypos = dstrect.y;
    gfx.hw.dst.rt.u32Width = dstrect.w;
    gfx.hw.dst.rt.u32Height = dstrect.h;
    
    if (target_surface) {
        gfx.hw.dst.surf = *target_surface;
    } else {
        /* Render directly to the active framebuffer (front or back) */
        gfx.hw.dst.surf.u32Width = GFX_GetFrameWidth();
        gfx.hw.dst.surf.u32Height = GFX_GetFrameHeight();
        gfx.hw.dst.surf.u32Stride = GFX_GetFrameStride();
        gfx.hw.dst.surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;
        gfx.hw.dst.surf.phyAddr = GFX_GetFrameBuffer();
    }

    /* Validate memory addresses, dimensions and stride alignment before BitBlit */
    if (gfx.hw.src.surf.phyAddr == 0 || gfx.hw.dst.surf.phyAddr == 0) {
        MMIYOO_LOG_ERROR("GFX_Copy: invalid MI_SYS addresses src=0x%llx dst=0x%llx",
                         (unsigned long long)gfx.hw.src.surf.phyAddr,
                         (unsigned long long)gfx.hw.dst.surf.phyAddr);
        return -1;
    }

    /* Validate stride alignment (MI_GFX requires 16-byte alignment) */
    if (gfx.hw.src.surf.u32Stride & 15) {
        MMIYOO_LOG_WARN("GFX_Copy: source stride %u not 16-byte aligned", gfx.hw.src.surf.u32Stride);
    }
    if (gfx.hw.dst.surf.u32Stride & 15) {
        MMIYOO_LOG_WARN("GFX_Copy: dest stride %u not 16-byte aligned", gfx.hw.dst.surf.u32Stride);
    }
    
    if (gfx.hw.src.rt.u32Width == 0 || gfx.hw.src.rt.u32Height == 0 ||
        gfx.hw.dst.rt.u32Width == 0 || gfx.hw.dst.rt.u32Height == 0) {
        MMIYOO_LOG_ERROR("GFX_Copy: zero dimension detected src=(%u,%u) dst=(%u,%u)",
                         gfx.hw.src.rt.u32Width,
                         gfx.hw.src.rt.u32Height,
                         gfx.hw.dst.rt.u32Width,
                         gfx.hw.dst.rt.u32Height);
        return -1;
    }

    result = MI_GFX_BitBlit(&gfx.hw.src.surf, &gfx.hw.src.rt, &gfx.hw.dst.surf, &gfx.hw.dst.rt, &gfx.hw.opt, &u16Fence);
    
    if (result == MI_SUCCESS) {
        // Add fence to global batch instead of waiting immediately
        GFX_AddTextureFence(u16Fence);
        return 0;
    }
    MMIYOO_LOG_WARN("GFX_Copy: MI_GFX_BitBlit failed (result=%d)", result);
    
    return -1;
#endif
}

// Framebuffer helpers

#endif /* SDL_VIDEO_DRIVER_MMIYOO */
