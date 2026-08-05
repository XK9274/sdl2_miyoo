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

GFX gfx = {0};
MMIYOO_VideoInfo MMiyooVideoInfo = {0};

static int g_framebuffer_width = 0;
static int g_framebuffer_height = 0;
static int g_framebuffer_stride = 0;
static int g_framebuffer_bytes_per_pixel = 4;
static int g_framebuffer_size = 0;
static int g_tmp_buffer_size = 0;
static Uint32 g_framebuffer_format = SDL_PIXELFORMAT_ARGB8888;

#define GFX_ACTION_NONE 0
#define GFX_ACTION_FLIP 1

static void
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

static int is_running = 0;
static SDL_Surface *cvt = NULL;
static SDL_SpinLock g_mmiyoo_sys_lock = 0;
static int g_mmiyoo_sys_refcount = 0;
static int g_mmiyoo_gfx_refcount = 0;

static void
mmiyoo_wait_gfx_idle(void)
{
    MI_GFX_WaitAllDone(TRUE, 0);
}

// Flush all pending texture fences for performance
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

extern MMIYOO_EventInfo MMiyooEventInfo;

static int MMIYOO_VideoInit(_THIS);
static int MMIYOO_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static void MMIYOO_VideoQuit(_THIS);

static void crash_handler(int sig) {
    void *array[10];
    size_t size;
    char **strings;

    size = backtrace(array, 10);
    strings = backtrace_symbols(array, size);

    if (strings != NULL) {
        free(strings);
    }

    // Reset signal to default and re-raise to get core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

static int video_handler(void *data)
{
    while (is_running) {
        SDL_LockMutex(gfx.action_mutex);
        
        // Wait for action signal instead of polling
        while (gfx.action == GFX_ACTION_NONE && is_running) {
            SDL_CondWait(gfx.action_cond, gfx.action_mutex);
        }
        
        if (gfx.action == GFX_ACTION_FLIP) {
            gfx.action = GFX_ACTION_NONE;
            SDL_UnlockMutex(gfx.action_mutex);

#ifdef MMIYOO
            if (gfx.thread.renderer && gfx.thread.texture) {
                SDL_Renderer *fb_renderer = gfx.thread.renderer;

                if (SDL_SetRenderTarget(fb_renderer, NULL) < 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                                "[MMIYOO] video_handler: SDL_SetRenderTarget(NULL) failed: %s",
                                SDL_GetError());
                } else {
                    SDL_FPoint default_center;

                    default_center.x = gfx.thread.drt.w * 0.5f;
                    default_center.y = gfx.thread.drt.h * 0.5f;

                    My_QueueCopy(fb_renderer,
                                 gfx.thread.texture,
                                 NULL,
                                 &gfx.thread.srt,
                                 &gfx.thread.drt,
                                 SDL_BLENDMODE_NONE,
                                 E_MI_GFX_ROTATE_0,
                                 SDL_FLIP_NONE,
                                 default_center);

                    SDL_RenderPresent(fb_renderer);
                }
            }
#endif
        } else {
            SDL_UnlockMutex(gfx.action_mutex);
        }
    }
    return 0;
}

#ifdef MMIYOO
int fb_init(void)
{
    MI_U32 frame_stride;
    MI_U32 frame_bytes;

    SDL_AtomicLock(&g_mmiyoo_sys_lock);
    if (g_mmiyoo_sys_refcount == 0) {
        MI_S32 ret = MI_SYS_Init();
        if (ret != MI_SUCCESS) {
            SDL_AtomicUnlock(&g_mmiyoo_sys_lock);
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                         "fb_init: MI_SYS_Init failed (0x%x)", ret);
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
                         "fb_init: MI_GFX_Open failed (0x%x)", ret);
            return -1;
        }
    }
    g_mmiyoo_gfx_refcount++;
    SDL_AtomicUnlock(&g_mmiyoo_sys_lock);

    gfx.double_buffer_enabled = SDL_TRUE;

    {
        const char *legacy_env = SDL_getenv("SDL_MMIYOO_DOUBLE_BUFFER");
        if (legacy_env && SDL_strcmp(legacy_env, "1") != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "SDL_MMIYOO_DOUBLE_BUFFER is deprecated; driver always uses double buffering");
        }
    }

    gfx.fb_dev = open("/dev/fb0", O_RDWR);
    ioctl(gfx.fb_dev, FBIOGET_FSCREENINFO, &gfx.finfo);
    ioctl(gfx.fb_dev, FBIOGET_VSCREENINFO, &gfx.vinfo);
    gfx.vinfo.yoffset = 0;
    gfx.vinfo.yres_virtual = gfx.vinfo.yres;
    ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);

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
    gfx.fb.length = (gfx.finfo.smem_len > 0) ? gfx.finfo.smem_len : frame_bytes;
    if (gfx.fb.length == 0) {
        gfx.fb.length = frame_bytes;
    }
    g_framebuffer_size = gfx.fb.length;
    g_tmp_buffer_size = frame_bytes;

    if (gfx.fb.phyAddr != 0 && gfx.fb.length > 0) {
        MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, gfx.fb.length);
        MI_SYS_Mmap(gfx.fb.phyAddr, gfx.fb.length, &gfx.fb.virAddr, TRUE);
    }
    
    memset(&gfx.hw.opt, 0, sizeof(gfx.hw.opt));

    if (MI_SYS_MMA_Alloc(NULL, g_tmp_buffer_size, &gfx.tmp.phyAddr) == MI_SUCCESS) {
        gfx.tmp.length = g_tmp_buffer_size;
        MI_SYS_Mmap(gfx.tmp.phyAddr, gfx.tmp.length, &gfx.tmp.virAddr, TRUE);
    }

    if (MI_SYS_MMA_Alloc(NULL, g_tmp_buffer_size, &gfx.overlay.phyAddr) == MI_SUCCESS) {
        gfx.overlay.length = g_tmp_buffer_size;
        MI_SYS_Mmap(gfx.overlay.phyAddr, gfx.overlay.length, &gfx.overlay.virAddr, TRUE);
    }

    if (gfx.double_buffer_enabled) {
        if (MI_SYS_MMA_Alloc(NULL, frame_bytes, &gfx.back.phyAddr) == MI_SUCCESS) {
            gfx.back.length = frame_bytes;
            if (MI_SYS_Mmap(gfx.back.phyAddr, gfx.back.length, &gfx.back.virAddr, TRUE) == MI_SUCCESS) {
                MI_SYS_MemsetPa(gfx.back.phyAddr, 0, gfx.back.length);
            } else {
                MMIYOO_LOG_WARN("fb_init: failed to map back buffer, disabling double buffer");
                MI_SYS_MMA_Free(gfx.back.phyAddr);
                gfx.back.phyAddr = 0;
                gfx.back.length = 0;
                gfx.back.virAddr = NULL;
                gfx.double_buffer_enabled = SDL_FALSE;
            }
        } else {
            MMIYOO_LOG_WARN("fb_init: failed to allocate back buffer, disabling double buffer");
            gfx.double_buffer_enabled = SDL_FALSE;
        }
    }

    return 0;
}

int fb_uninit(void)
{
    mmiyoo_wait_gfx_idle();

    if (gfx.back.virAddr && gfx.back.length > 0) {
        MI_SYS_Munmap(gfx.back.virAddr, gfx.back.length);
        gfx.back.virAddr = NULL;
    }
    if (gfx.back.phyAddr) {
        MI_SYS_MMA_Free(gfx.back.phyAddr);
        gfx.back.phyAddr = 0;
        gfx.back.length = 0;
    }

    if (gfx.fb.virAddr && gfx.fb.length > 0) {
        MI_SYS_Munmap(gfx.fb.virAddr, gfx.fb.length);
        gfx.fb.virAddr = NULL;
    }
    if (gfx.tmp.virAddr && gfx.tmp.length > 0) {
        MI_SYS_Munmap(gfx.tmp.virAddr, gfx.tmp.length);
        gfx.tmp.virAddr = NULL;
    }
    if (gfx.tmp.phyAddr) {
        MI_SYS_MMA_Free(gfx.tmp.phyAddr);
        gfx.tmp.phyAddr = 0;
        gfx.tmp.length = 0;
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

    SDL_AtomicLock(&g_mmiyoo_sys_lock);
    if (g_mmiyoo_gfx_refcount > 0) {
        g_mmiyoo_gfx_refcount--;
        if (g_mmiyoo_gfx_refcount == 0) {
            mmiyoo_wait_gfx_idle();
            MI_GFX_Close();
        }
    }

    if (g_mmiyoo_sys_refcount > 0) {
        g_mmiyoo_sys_refcount--;
        if (g_mmiyoo_sys_refcount == 0) {
            MI_S32 ret = MI_SYS_Exit();
            if (ret != MI_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "fb_uninit: MI_SYS_Exit returned 0x%x", ret);
            }
        }
    }
    SDL_AtomicUnlock(&g_mmiyoo_sys_lock);
    return 0;
}
#endif

void GFX_Init(void)
{
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGABRT, crash_handler);

    if (fb_init() != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                     "GFX_Init: fb_init failed");
        return;
    }
    
    // Initialize SDL synchronization primitives
    gfx.action_mutex = SDL_CreateMutex();
    gfx.action_cond = SDL_CreateCond();
    if (!gfx.action_mutex || !gfx.action_cond) {
        return;
    }
    
    SDL_zero(gfx.thread);
    {
        MI_PHY phy_addr;
        if (MI_SYS_MMA_Alloc(NULL, g_tmp_buffer_size, &phy_addr) == MI_SUCCESS) {
            if (MI_SYS_Mmap(phy_addr, g_tmp_buffer_size, &gfx.thread.pixels, TRUE) == MI_SUCCESS) {
                gfx.thread.phy_addr = phy_addr;
                gfx.thread.size = g_tmp_buffer_size;
            } else {
                MI_SYS_MMA_Free(phy_addr);
            }
        }
        if (!gfx.thread.pixels) {
            gfx.thread.pixels = SDL_malloc(g_tmp_buffer_size);
            gfx.thread.phy_addr = 0;
            gfx.thread.size = g_tmp_buffer_size;
        }
        if (!gfx.thread.pixels) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "GFX_Init: failed to allocate staging pixels buffer");
            return;
        }
    }

    cvt = SDL_CreateRGBSurface(SDL_SWSURFACE,
                               g_framebuffer_width > 0 ? g_framebuffer_width : MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH,
                               g_framebuffer_height > 0 ? g_framebuffer_height : MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT,
                               32, 0, 0, 0, 0);

    is_running = 1;
    gfx.action = GFX_ACTION_NONE;
    
    // Create SDL thread instead of pthread
    gfx.video_thread = SDL_CreateThread(video_handler, "MMIYOOVideoThread", NULL);
    if (!gfx.video_thread) {
        return;
    }
}

void GFX_Quit(void)
{
    is_running = 0;
    
    // Signal the thread to wake up and exit
    if (gfx.action_mutex && gfx.action_cond) {
        SDL_LockMutex(gfx.action_mutex);
        SDL_CondSignal(gfx.action_cond);
        SDL_UnlockMutex(gfx.action_mutex);
    }
    
    // Wait for thread to complete
    if (gfx.video_thread) {
        SDL_WaitThread(gfx.video_thread, NULL);
        gfx.video_thread = NULL;
    }
    
    GFX_Clear();

    fb_uninit();
    
    // Properly cleanup MI_SYS allocated memory
    if (gfx.thread.pixels) {
        if (gfx.thread.phy_addr != 0) {
            MI_SYS_Munmap(gfx.thread.pixels, gfx.thread.size);
            MI_SYS_MMA_Free(gfx.thread.phy_addr);
        } else {
            SDL_free(gfx.thread.pixels);
        }
        gfx.thread.pixels = NULL;
        gfx.thread.phy_addr = 0;
        gfx.thread.size = 0;
    }
    
    // Cleanup SDL synchronization primitives
    if (gfx.action_cond) {
        SDL_DestroyCond(gfx.action_cond);
        gfx.action_cond = NULL;
    }
    if (gfx.action_mutex) {
        SDL_DestroyMutex(gfx.action_mutex);
        gfx.action_mutex = NULL;
    }

    // Single buffer mode - no yoffset to reset
    close(gfx.fb_dev);
    gfx.fb_dev = 0;

    if (cvt) {
        SDL_FreeSurface(cvt);
        cvt = NULL;
    }
}

void GFX_Clear(void)
{
#ifdef MMIYOO
    if (gfx.fb.phyAddr && gfx.fb.length > 0) {
        MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, gfx.fb.length);
    }
    if (gfx.double_buffer_enabled && gfx.back.phyAddr && gfx.back.length > 0) {
        MI_SYS_MemsetPa(gfx.back.phyAddr, 0, gfx.back.length);
    }
    if (gfx.tmp.phyAddr && gfx.tmp.length > 0) {
        MI_SYS_MemsetPa(gfx.tmp.phyAddr, 0, gfx.tmp.length);
    }
#endif
}


int GFX_Copy(const void *pixels,
             MI_PHY pixels_phy,
             SDL_Rect srcrect,
             SDL_Rect dstrect,
             int pitch,
             int alpha,
             int rotate,
             MI_GFX_Mirror_e mirror,
             SDL_BlendMode blend_mode,
             MI_GFX_Surface_t *target_surface,
             const SDL_Rect *clip_rect,
             SDL_bool clip_enabled,
             Uint32 src_format,
             MI_GFX_ColorFmt_e src_mi_format,
             Uint32 bytes_per_pixel)
{
#ifdef MMIYOO
    SDL_bool use_staging_copy = SDL_TRUE;
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
    
    /* Validate format consistency */
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

    if (pixels_phy != 0) {
        use_staging_copy = SDL_FALSE;
    }

    if (use_staging_copy) {
        size_t row_bytes;
        size_t aligned_stride;
        size_t copy_size;
        const Uint8 *src_row;
        Uint8 *dst_row;

#ifdef MMIYOO
        /* Ensure previous hardware blits that consumed the shared staging buffer
           have finished before we overwrite it with new pixel data. */
        GFX_FlushTextureFences();
#endif

        row_bytes = srcrect.w * src_bytes_per_pixel;
        aligned_stride = (row_bytes + 15) & ~15;  /* 16-byte alignment for MI_GFX */
        copy_size = srcrect.h * row_bytes;

        if (row_bytes == 0 || srcrect.h == 0) {
            MMIYOO_LOG_ERROR("GFX_Copy: zero-sized copy (row_bytes=%zu height=%d)", row_bytes, srcrect.h);
            return -1;
        }

        /* Validate copy size */
        if ((int)copy_size > g_tmp_buffer_size) {
            MMIYOO_LOG_ERROR("GFX_Copy: copy_size=%zu exceeds staging buffer (%d)", copy_size, g_tmp_buffer_size);
            return -1;
        }

        /* Pure MI_SYS strategy: external pixel data always uses neon_memcpy to MI_SYS staging buffer */
        /* DMA optimization happens between MI_SYS buffers in hardware operations */

        src_row = (const Uint8 *)pixels + (size_t)srcrect.y * pitch + (size_t)srcrect.x * src_bytes_per_pixel;
        dst_row = (Uint8 *)gfx.tmp.virAddr;

        for (int row = 0; row < srcrect.h; row++) {
            neon_memcpy(dst_row, src_row, row_bytes);
            src_row += pitch;
            dst_row += aligned_stride;  /* Use aligned stride for staging buffer */
        }

        MI_SYS_FlushInvCache(gfx.tmp.virAddr, srcrect.h * aligned_stride);
    }

    /* Configure blending according to SDL's requested mode */
    gfx.hw.opt.u32GlobalSrcConstColor = 0;
    gfx.hw.opt.u32GlobalDstConstColor = 0;
    gfx.hw.opt.eRotate = rotate;
    gfx.hw.opt.eMirror = mirror;

    /* Disable colorkey operations for predictable blending */
    gfx.hw.opt.stSrcColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */
    gfx.hw.opt.stDstColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */

    /* Blend-factor mapping follows SigmaStar docs (see
     *   GFX - SigmaStarDocs copy.txt §3.9/3.10 for DfbBldOp_e and DfbBlendFlags_e
     * and SDL's composed modes in src/render/SDL_render.c). */
    switch (blend_mode) {
        case SDL_BLENDMODE_NONE:
            gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
            gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
            gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
            break;
        case SDL_BLENDMODE_ADD:
            gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_SRCALPHA;
            gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
            gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
            break;
        case SDL_BLENDMODE_MOD:
            gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
            gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_SRCCOLOR;
            gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
            break;
        case SDL_BLENDMODE_MUL:
            gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_DESTCOLOR;
            gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
            gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
            break;
        default:
            /* SDL_BLENDMODE_BLEND and any unknown modes fall back to standard alpha blending */
            gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_SRCALPHA;
            gfx.hw.opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
            gfx.hw.opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
            break;
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
    /* Setup source surface and rectangle */
    gfx.hw.src.surf.eColorFmt = mi_src_format;

    if (use_staging_copy) {
        gfx.hw.src.rt.s32Xpos = 0;  /* Always 0 for staging buffer */
        gfx.hw.src.rt.s32Ypos = 0;  /* Always 0 for staging buffer */
        gfx.hw.src.rt.u32Width = srcrect.w;
        gfx.hw.src.rt.u32Height = srcrect.h;
        gfx.hw.src.surf.u32Width = srcrect.w;
        gfx.hw.src.surf.u32Height = srcrect.h;
        gfx.hw.src.surf.u32Stride = (srcrect.w * src_bytes_per_pixel + 15) & ~15;  /* 16-byte aligned stride */
        gfx.hw.src.surf.phyAddr = gfx.tmp.phyAddr;
    } else {
        MI_PHY offset;
        const Uint8 *cpu_src;

        offset = (MI_PHY)srcrect.y * pitch + (MI_PHY)srcrect.x * src_bytes_per_pixel;
        cpu_src = (const Uint8 *)pixels + (size_t)srcrect.y * pitch + (size_t)srcrect.x * src_bytes_per_pixel;

        (void)cpu_src; /* currently unused but kept for future cache management */

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
        if (global_fence_count < MAX_GLOBAL_FENCES) {
            global_texture_fences[global_fence_count++] = u16Fence;
        }
        return 0;
    }
    MMIYOO_LOG_WARN("GFX_Copy: MI_GFX_BitBlit failed (result=%d)", result);
    
    return -1;
#endif
}

// Framebuffer helpers
MI_PHY GFX_GetFrameBuffer(void)
{
#ifdef MMIYOO
    if (gfx.double_buffer_enabled && gfx.back.phyAddr != 0) {
        return gfx.back.phyAddr;
    }
    return gfx.fb.phyAddr;
#else
    return 0;
#endif
}

MI_U32 GFX_GetFrameStride(void)
{
#ifdef MMIYOO
    if (g_framebuffer_stride > 0) {
        return (MI_U32)g_framebuffer_stride;
    }
    return (MI_U32)(MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH * MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL);
#else
    return 0;
#endif
}

MI_U32 GFX_GetFrameWidth(void)
{
#ifdef MMIYOO
    if (g_framebuffer_width > 0) {
        return (MI_U32)g_framebuffer_width;
    }
    return MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
#else
    return 0;
#endif
}

MI_U32 GFX_GetFrameHeight(void)
{
#ifdef MMIYOO
    if (g_framebuffer_height > 0) {
        return (MI_U32)g_framebuffer_height;
    }
    return MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
#else
    return 0;
#endif
}

SDL_bool GFX_IsDoubleBuffered(void)
{
#ifdef MMIYOO
    return gfx.double_buffer_enabled;
#else
    return SDL_FALSE;
#endif
}

void GFX_SwapBuffers(void)
{
#ifdef MMIYOO
    MI_U32 copy_bytes;

    if (!gfx.double_buffer_enabled || gfx.back.phyAddr == 0 || gfx.fb.phyAddr == 0) {
        return;
    }

    copy_bytes = gfx.back.length;
    if (copy_bytes == 0) {
        copy_bytes = g_framebuffer_size;
    }
    if (gfx.fb.length > 0 && gfx.fb.length < copy_bytes) {
        copy_bytes = gfx.fb.length;
    }
    if (copy_bytes == 0) {
        return;
    }

    if (gfx.back.virAddr) {
        MI_SYS_FlushInvCache(gfx.back.virAddr, copy_bytes);
    }

    if (MI_SYS_MemcpyPa(gfx.fb.phyAddr, gfx.back.phyAddr, copy_bytes) != MI_SUCCESS) {
        MMIYOO_LOG_WARN("GFX_SwapBuffers: MI_SYS_MemcpyPa failed (bytes=%u)", copy_bytes);
    }
#endif
}

void *GFX_GetFrameBufferVirtual(void)
{
#ifdef MMIYOO
    if (gfx.double_buffer_enabled && gfx.back.virAddr) {
        return gfx.back.virAddr;
    }
    return gfx.fb.virAddr;
#else
    return NULL;
#endif
}

static int MMIYOO_Available(void)
{
    const char *envr = SDL_getenv("SDL_VIDEODRIVER");
    if((envr) && (SDL_strcmp(envr, MMIYOO_DRIVER_NAME) == 0)) {
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

#endif
