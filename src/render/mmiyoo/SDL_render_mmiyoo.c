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

#if SDL_VIDEO_RENDER_MMIYOO

#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <sys/ioctl.h>
#include <math.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "SDL_assert.h"
#include "SDL_hints.h"
#include "SDL_log.h"
#include "SDL_stdinc.h"
#include "../SDL_sysrender.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "../../video/mmiyoo/SDL_video_mmiyoo.h"
#include "../../video/mmiyoo/SDL_event_mmiyoo.h"
#include "SDL_rect.h"
#include "SDL_timer.h"
#include "neon.h"
#include "SDL_render_mmiyoo_internal.h"

/* Renderer driver descriptor and vtable wiring, renderer creation/
 * destruction, and top-level lifecycle/dispatch. Delegates to geometry.c,
 * texture.c, commands.c, scaling.c, and present.c for everything else. */

SDL_bool mmiyoo_debug_verbose = SDL_FALSE;

static void MMIYOO_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
}

static int MMIYOO_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;

    if (w) {
        *w = MMIYOO_GetFramebufferWidth(data);
    }
    if (h) {
        *h = MMIYOO_GetFramebufferHeight(data);
    }
    return 0;
}

static int MMIYOO_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    data->boundTarget = texture;

    MMIYOO_VERBOSE_LOG("SetRenderTarget: texture=%p", (void *)texture);

    if (texture) {
        MMIYOO_TextureData *texture_data = (MMIYOO_TextureData *)texture->driverdata;
        if (texture_data) {
            data->current_target_surface = texture_data->gfx_surface;
            data->is_target_texture = SDL_TRUE;
            MMIYOO_VERBOSE_LOG("SetRenderTarget: texture surface phy=0x%llx stride=%u size=%ux%u",
                               (unsigned long long)texture_data->gfx_surface.phyAddr,
                               texture_data->gfx_surface.u32Stride,
                               texture_data->gfx_surface.u32Width,
                               texture_data->gfx_surface.u32Height);
        } else {
            return -1;
        }
    } else {
        memset(&data->current_target_surface, 0, sizeof(MI_GFX_Surface_t));
        data->current_target_surface.u32Width = (MI_U32)MMIYOO_GetFramebufferWidth(data);
        data->current_target_surface.u32Height = (MI_U32)MMIYOO_GetFramebufferHeight(data);
        data->current_target_surface.u32Stride = GFX_GetFrameStride();
        data->current_target_surface.eColorFmt = E_MI_GFX_FMT_ARGB8888;

        data->current_target_surface.phyAddr = GFX_GetFrameBuffer();
        data->is_target_texture = SDL_FALSE;
        data->framebuffer_width = (int)data->current_target_surface.u32Width;
        data->framebuffer_height = (int)data->current_target_surface.u32Height;
        MMIYOO_VERBOSE_LOG("SetRenderTarget: framebuffer phy=0x%llx stride=%u",
                           (unsigned long long)data->current_target_surface.phyAddr,
                           data->current_target_surface.u32Stride);
    }

    /* Reset clip tracking when target changes to avoid stale rectangles */
    MMIYOO_UpdateClipState(data, SDL_FALSE, NULL);
    data->viewport_enabled = SDL_FALSE;

    return 0;
}

/* Gates SDL_Set{Render,Texture}BlendMode for composed modes; mirrors GFX_Copy's own matching-factors/ADD-only check. */
static SDL_bool MMIYOO_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    (void)renderer;

    if (colorOperation != SDL_BLENDOPERATION_ADD || alphaOperation != SDL_BLENDOPERATION_ADD) {
        return SDL_FALSE;
    }
    if (srcColorFactor != srcAlphaFactor || dstColorFactor != dstAlphaFactor) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

static void MMIYOO_DestroyRenderer(SDL_Renderer *renderer)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;

    if (data) {
        if (data->initialized) {
            MI_GFX_WaitAllDone(TRUE, 0);
            data->initialized = SDL_FALSE;
        }
        if (data->scale_scratch_vir) {
            MI_SYS_Munmap(data->scale_scratch_vir, data->scale_scratch_alloc_size);
            MI_SYS_MMA_Free(data->scale_scratch_phy);
        }
        SDL_free(data);
    }

    /* Never leave cached MMA blocks dangling past renderer teardown. */
    MMIYOO_TexturePoolDrain();

    SDL_free(renderer);
}

SDL_Renderer *MMIYOO_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    int pixelformat = 0;
    SDL_Renderer *renderer = NULL;
    MMIYOO_RenderData *data = NULL;
    const char *debug_hint;
    const char *line_hint;

    /* Force SDL to prefer hardware line rendering when no explicit hint is set. */
    line_hint = SDL_GetHint(SDL_HINT_RENDER_LINE_METHOD);
    if (!line_hint || !*line_hint) {
        SDL_SetHint(SDL_HINT_RENDER_LINE_METHOD, "2");
    }

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if(!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (MMIYOO_RenderData *) SDL_calloc(1, sizeof(*data));
    if(!data) {
        MMIYOO_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }

    renderer->WindowEvent = MMIYOO_WindowEvent;
    renderer->CreateTexture = MMIYOO_CreateTexture;
    renderer->UpdateTexture = MMIYOO_UpdateTexture;
    renderer->LockTexture = MMIYOO_LockTexture;
    renderer->UnlockTexture = MMIYOO_UnlockTexture;
    renderer->SetTextureScaleMode = MMIYOO_SetTextureScaleMode;
    renderer->SetRenderTarget = MMIYOO_SetRenderTarget;
    renderer->QueueSetViewport = MMIYOO_QueueSetViewport;
    renderer->QueueSetDrawColor = MMIYOO_QueueSetDrawColor;
    renderer->QueueDrawPoints = MMIYOO_QueueDrawPoints;
    renderer->QueueDrawLines = MMIYOO_QueueDrawLines;
    renderer->QueueGeometry = MMIYOO_QueueGeometry;
    renderer->QueueFillRects = MMIYOO_QueueFillRects;
    renderer->QueueCopy = MMIYOO_QueueCopy;
    renderer->QueueCopyEx = MMIYOO_QueueCopyEx;
    renderer->RunCommandQueue = MMIYOO_RunCommandQueue;
    renderer->RenderReadPixels = MMIYOO_RenderReadPixels;
    renderer->RenderPresent = MMIYOO_RenderPresent;
    renderer->GetOutputSize = MMIYOO_GetOutputSize;
    renderer->DestroyTexture = MMIYOO_DestroyTexture;
    renderer->DestroyRenderer = MMIYOO_DestroyRenderer;
    renderer->SetVSync = MMIYOO_SetVSync;
    renderer->SupportsBlendMode = MMIYOO_SupportsBlendMode;
    renderer->info = MMIYOO_RenderDriver.info;
    renderer->info.flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    renderer->line_method = SDL_RENDERLINEMETHOD_LINES;
    renderer->driverdata = data;
    renderer->window = window;

    SDL_LogSetPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_INFO);

    debug_hint = SDL_GetHint("SDL_MMIYOO_DEBUG");
    if (debug_hint && SDL_atoi(debug_hint) != 0) {
        SDL_LogSetPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_DEBUG);
        SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
        mmiyoo_debug_verbose = SDL_TRUE;
    }

    if (!mmiyoo_debug_verbose) {
        const char *verbose_hint = SDL_GetHint("SDL_MMIYOO_DEBUG_VERBOSE");
        if (verbose_hint && SDL_atoi(verbose_hint) != 0) {
            mmiyoo_debug_verbose = SDL_TRUE;
        }
    }

    {
        const char *geom_stats_hint = SDL_GetHint("SDL_MMIYOO_GEOMETRY_STATS");
        if (geom_stats_hint && SDL_atoi(geom_stats_hint) != 0) {
            data->collect_span_stats = SDL_TRUE;
        }
    }

    {
        const char *timing_hint = SDL_GetHint("SDL_MMIYOO_FRAME_TIMING");
        if (timing_hint && SDL_atoi(timing_hint) != 0) {
            data->collect_frame_timing = SDL_TRUE;
        }
    }

    {
        const char *quickpath_hint = SDL_GetHint("SDL_MMIYOO_GEOMETRY_QUICKPATH");
        if (quickpath_hint && SDL_atoi(quickpath_hint) != 0) {
            data->geometry_quickpath_enabled = SDL_TRUE;
        }
    }

    /* On by default; set SDL_MMIYOO_INTEGER_SCALE=0 to fall back to unscaled-blit-only behavior. */
    data->integer_scale_enabled = SDL_TRUE;
    {
        const char *integer_scale_hint = SDL_GetHint("SDL_MMIYOO_INTEGER_SCALE");
        if (integer_scale_hint && SDL_atoi(integer_scale_hint) == 0) {
            data->integer_scale_enabled = SDL_FALSE;
        }
    }

    {
        SDL_bool pool_enabled = SDL_TRUE;
        size_t pool_max_bytes = MMIYOO_TEXTURE_POOL_DEFAULT_MAX_BYTES;
        const char *pool_hint = SDL_GetHint("SDL_MMIYOO_TEXTURE_POOL");
        const char *pool_bytes_hint = SDL_GetHint("SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES");

        if (pool_hint) {
            pool_enabled = (SDL_atoi(pool_hint) != 0) ? SDL_TRUE : SDL_FALSE;
        }
        if (pool_bytes_hint) {
            int v = SDL_atoi(pool_bytes_hint);
            if (v > 0) {
                pool_max_bytes = (size_t)v;
            }
        }
        MMIYOO_TexturePoolConfigure(pool_enabled, pool_max_bytes);
    }

    data->span_band_height = 3;
    {
        const char *band_hint = SDL_GetHint("SDL_MMIYOO_GEOMETRY_BAND_HEIGHT");
        if (band_hint) {
            int band = SDL_atoi(band_hint);
            if (band < 1) {
                band = 1;
            } else if (band > 32) {
                band = 32;
            }
            data->span_band_height = (Uint8)band;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                "MMIYOO geometry band height=%d",
                (int)data->span_band_height);

    g_warned_copyex_angle = SDL_FALSE;

    if(data->initialized != SDL_FALSE) {
        return 0;
    }
    data->initialized = SDL_TRUE;

    data->framebuffer_width = (int)GFX_GetFrameWidth();
    data->framebuffer_height = (int)GFX_GetFrameHeight();
    if (data->framebuffer_width <= 0 || data->framebuffer_height <= 0) {
        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        if (window_w > 0) {
            data->framebuffer_width = window_w;
        }
        if (window_h > 0) {
            data->framebuffer_height = window_h;
        }
        if (data->framebuffer_width <= 0) {
            data->framebuffer_width = MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
        }
        if (data->framebuffer_height <= 0) {
            data->framebuffer_height = MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
        }
    }

    if (SDL_GetHintBoolean("SDL_MMIYOO_DEBUG_LOG", SDL_FALSE)) {
        MMIYOO_LOG_WARN("SCALEDBG RendererInit: GFX_GetFrameWidth/Height=%dx%d final framebuffer_width/height=%dx%d",
                        (int)GFX_GetFrameWidth(), (int)GFX_GetFrameHeight(),
                        data->framebuffer_width, data->framebuffer_height);
    }

    memset(&data->current_target_surface, 0, sizeof(MI_GFX_Surface_t));
    data->current_target_surface.u32Width = (MI_U32)data->framebuffer_width;
    data->current_target_surface.u32Height = (MI_U32)data->framebuffer_height;
    data->current_target_surface.u32Stride = GFX_GetFrameStride();
    data->current_target_surface.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    data->current_target_surface.phyAddr = GFX_GetFrameBuffer();
    data->is_target_texture = SDL_FALSE;
    data->texture_blitted_to_screen = SDL_FALSE;

    // Initialize viewport to full screen
    data->viewport.x = 0;
    data->viewport.y = 0;
    data->viewport.w = data->framebuffer_width;
    data->viewport.h = data->framebuffer_height;
    data->viewport_enabled = SDL_FALSE;
    data->clip_enabled = SDL_FALSE;
    SDL_zero(data->clip_rect);

    // Initialize draw color to white (default SDL behavior)
    data->draw_color_r = 255;
    data->draw_color_g = 255;
    data->draw_color_b = 255;
    data->draw_color_a = 255;

    if(flags & SDL_RENDERER_PRESENTVSYNC) {
        data->vsync = SDL_TRUE;
    }
    else {
        data->vsync = SDL_FALSE;
    }
    MMIYOO_UpdatePresentVSyncFlag(renderer, data->vsync);

    pixelformat = SDL_GetWindowPixelFormat(window);
    switch(pixelformat) {
    case SDL_PIXELFORMAT_RGB565:
        data->bpp = 2;
        break;
    case SDL_PIXELFORMAT_ARGB8888:
        data->bpp = 4;
        break;
    }
    return renderer;
}

SDL_RenderDriver MMIYOO_RenderDriver = {
    .CreateRenderer = MMIYOO_CreateRenderer,
    .info = {
        .name = "MMIYOO",
        .flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
        .num_texture_formats = 9,
        .texture_formats = {
            [0] = SDL_PIXELFORMAT_RGB565,
            [1] = SDL_PIXELFORMAT_BGR565,
            [2] = SDL_PIXELFORMAT_ARGB8888,
            [3] = SDL_PIXELFORMAT_RGBA8888,
            [4] = SDL_PIXELFORMAT_ABGR8888,
            [5] = SDL_PIXELFORMAT_BGRA8888,
            [6] = SDL_PIXELFORMAT_ARGB1555,
            [7] = SDL_PIXELFORMAT_ARGB4444,
            [8] = SDL_PIXELFORMAT_RGBA4444,
        },
        .max_texture_width = 4096,
        .max_texture_height = 4096,
    }
};

#endif /* SDL_VIDEO_RENDER_MMIYOO */
