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

/* Render-target readback, present-vsync selection, present timing, and
 * renderer presentation. */

int MMIYOO_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 pixel_format, void *pixels, int pitch)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MMIYOO_TextureData *src_texture;
    void *src_pixels;

    /* Only a render-target texture has a CPU-mapped source of known pixel
     * format to read back. The default/window target's only CPU-visible
     * buffer has a runtime pixel format that was never verified on-device,
     * so it stays unsupported rather than risk silently returning
     * corrupted pixels. */
    if (!data->is_target_texture || !data->boundTarget) {
        return SDL_Unsupported();
    }

    src_texture = (MMIYOO_TextureData *)data->boundTarget->driverdata;
    if (!src_texture || !src_texture->virAddr) {
        return SDL_Unsupported();
    }

    if (rect->x < 0 || rect->y < 0 ||
        (unsigned int)(rect->x + rect->w) > src_texture->width ||
        (unsigned int)(rect->y + rect->h) > src_texture->height) {
        return SDL_SetError("Tried to read outside of texture bounds");
    }

    src_pixels = (Uint8 *)src_texture->virAddr +
                 rect->y * src_texture->pitch +
                 rect->x * src_texture->bytes_per_pixel;

    return SDL_ConvertPixels(rect->w, rect->h,
                              src_texture->format, src_pixels, src_texture->pitch,
                              pixel_format, pixels, pitch);
}

static SDL_bool
MMIYOO_PresentVSyncActive(SDL_bool renderer_vsync_requested)
{
    const MMIYOO_VSyncMode_e mode = MMIYOO_ResolvePresentVSyncMode(renderer_vsync_requested);
    if (mode == MMIYOO_VSYNC_MODE_OFF) {
        return SDL_FALSE;
    }
    if (mode == MMIYOO_VSYNC_MODE_STRICT) {
        return GFX_IsPageFlipEnabled();
    }
    return SDL_TRUE; /* adaptive: FBIO_WAITFORVSYNC genuinely executes each present */
}

void
MMIYOO_UpdatePresentVSyncFlag(SDL_Renderer *renderer, SDL_bool renderer_vsync_requested)
{
    if (MMIYOO_PresentVSyncActive(renderer_vsync_requested)) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    } else {
        renderer->info.flags &= ~SDL_RENDERER_PRESENTVSYNC;
    }
}

/* Computes the completed window's per-frame averages into both the log line
 * and the SDL_MMIYOO_GetFrameTimingStats cache, so the two never drift. */
static void
MMIYOO_ComputeFrameTimingWindow(MMIYOO_RenderData *data, Uint64 freq, double window_s)
{
    const double frame_count = (double)data->timing_frames;
    SDL_MMIYOO_FrameTimingStats stats;

    stats.fps = frame_count / window_s;
    stats.cmdqueue_ms_per_frame = (double)data->timing_command_queue_ticks * 1000.0 / (double)freq / frame_count;
    stats.present_ms_per_frame = (double)data->timing_present_ticks * 1000.0 / (double)freq / frame_count;
    stats.blits_per_frame = (double)data->timing_blit_calls / frame_count;
    stats.fill_ms_per_frame = (double)data->timing_fill_ticks * 1000.0 / (double)freq / frame_count;
    stats.copy_ms_per_frame = (double)data->timing_copy_ticks * 1000.0 / (double)freq / frame_count;
    stats.geometry_ms_per_frame = (double)data->timing_geometry_ticks * 1000.0 / (double)freq / frame_count;
    stats.lines_ms_per_frame = (double)data->timing_lines_ticks * 1000.0 / (double)freq / frame_count;
    stats.misc_ms_per_frame = (double)data->timing_misc_ticks * 1000.0 / (double)freq / frame_count;

    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                "MMIYOO frame timing: fps=%.1f cmdQueue=%.2fms/frame present=%.2fms/frame blits=%.1f/frame",
                stats.fps, stats.cmdqueue_ms_per_frame, stats.present_ms_per_frame, stats.blits_per_frame);
    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                "MMIYOO cmdQueue breakdown: fill=%.2fms copy=%.2fms geometry=%.2fms lines=%.2fms misc=%.2fms (ms/frame)",
                stats.fill_ms_per_frame, stats.copy_ms_per_frame, stats.geometry_ms_per_frame,
                stats.lines_ms_per_frame, stats.misc_ms_per_frame);

    data->timing_stats_cache = stats;
    data->timing_stats_valid = SDL_TRUE;
}

static MMIYOO_RenderData *
MMIYOO_GetRenderDataIfMMIYOO(SDL_Renderer *renderer)
{
    if (!renderer) {
        return NULL;
    }
    if (SDL_strcmp(renderer->info.name, "MMIYOO") != 0) {
        return NULL;
    }
    return (MMIYOO_RenderData *)renderer->driverdata;
}

SDL_bool
SDL_MMIYOO_GetFrameTimingStats(SDL_Renderer *renderer, SDL_MMIYOO_FrameTimingStats *out)
{
    MMIYOO_RenderData *data = MMIYOO_GetRenderDataIfMMIYOO(renderer);
    if (!data || !out || !data->collect_frame_timing || !data->timing_stats_valid) {
        return SDL_FALSE;
    }
    *out = data->timing_stats_cache;
    return SDL_TRUE;
}

SDL_bool
SDL_MMIYOO_GetGeometryStats(SDL_Renderer *renderer, SDL_MMIYOO_GeometryStats *out)
{
    MMIYOO_RenderData *data = MMIYOO_GetRenderDataIfMMIYOO(renderer);
    if (!data || !out || !data->collect_span_stats || !data->geometry_stats_valid) {
        return SDL_FALSE;
    }
    *out = data->geometry_stats_cache;
    return SDL_TRUE;
}

void MMIYOO_RenderPresent(SDL_Renderer *renderer)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MI_GFX_Surface_t target_surface_before;
    SDL_bool was_target_texture;

    if (!data->logical_size_applied) {
        int window_w = 0;
        int window_h = 0;

        /* Deferred to first present, not done in MMIYOO_CreateRenderer:
         * SDL only marks renderer->magic valid after CreateRenderer
         * returns, so SDL_RenderSetLogicalSize (magic-guarded) would fail
         * if called from inside it. By present time the command queue has
         * already been flushed by SDL_RenderPresent, so calling back into
         * SDL_RenderSetLogicalSize here is safe. */
        data->logical_size_applied = SDL_TRUE;
        SDL_GetWindowSize(renderer->window, &window_w, &window_h);

        if (window_w > 0 && window_h > 0 &&
            (window_w > data->framebuffer_width || window_h > data->framebuffer_height)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_RENDER,
                         "SCALEDBG RenderPresent: window=%dx%d exceeds framebuffer=%dx%d, applying SDL_RenderSetLogicalSize",
                         window_w, window_h, data->framebuffer_width, data->framebuffer_height);
            if (SDL_RenderSetLogicalSize(renderer, window_w, window_h) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                            "MMIYOO: SDL_RenderSetLogicalSize(%d,%d) failed: %s",
                            window_w, window_h, SDL_GetError());
            }
        }
    }

    target_surface_before = data->current_target_surface;
    was_target_texture = data->is_target_texture;

    SDL_LogDebug(SDL_LOG_CATEGORY_RENDER,
                 "MMIYOO_RenderPresent: targetTex=%d fb=%ux%u viewport=%dx%d at %d,%d",
                 data->is_target_texture ? 1 : 0,
                 (unsigned int)data->framebuffer_width,
                 (unsigned int)data->framebuffer_height,
                 data->viewport.w, data->viewport.h,
                 data->viewport.x, data->viewport.y);

    {
        Uint64 present_timing_start = data->collect_frame_timing ? SDL_GetPerformanceCounter() : 0;

        GFX_FlushTextureFences();

        /* Flushes the CPU write(s) this frame made before the swap below lets
         * hardware read that memory. The single-buffer fallback has no other
         * flush point, so this is load-bearing there, not just redundant. */
        MMIYOO_FlushDirectWriteDirty(data);
        data->direct_write_used_this_frame = SDL_FALSE;

        if (!data->is_target_texture || data->texture_blitted_to_screen) {
            GFX_SwapBuffers(data->vsync);
            MMIYOO_UpdatePresentVSyncFlag(renderer, data->vsync);
            if (!was_target_texture) {
                data->current_target_surface.phyAddr = GFX_GetFrameBuffer();
                data->current_target_surface.u32Stride = GFX_GetFrameStride();
                data->current_target_surface.u32Width = GFX_GetFrameWidth();
                data->current_target_surface.u32Height = GFX_GetFrameHeight();
                data->framebuffer_width = (int)data->current_target_surface.u32Width;
                data->framebuffer_height = (int)data->current_target_surface.u32Height;
            } else {
                data->current_target_surface = target_surface_before;
                data->is_target_texture = SDL_TRUE;
            }
            data->texture_blitted_to_screen = SDL_FALSE;
        }

        if (data->collect_frame_timing) {
            Uint64 now = SDL_GetPerformanceCounter();
            Uint64 freq = SDL_GetPerformanceFrequency();

            data->timing_present_ticks += now - present_timing_start;
            data->timing_frames += 1;

            if (data->timing_window_start_ticks == 0) {
                data->timing_window_start_ticks = now;
            } else if (now - data->timing_window_start_ticks >= freq && data->timing_frames > 0) {
                double window_s = (double)(now - data->timing_window_start_ticks) / (double)freq;

                MMIYOO_ComputeFrameTimingWindow(data, freq, window_s);

                data->timing_command_queue_ticks = 0;
                data->timing_present_ticks = 0;
                data->timing_blit_calls = 0;
                data->timing_frames = 0;
                data->timing_fill_ticks = 0;
                data->timing_copy_ticks = 0;
                data->timing_geometry_ticks = 0;
                data->timing_lines_ticks = 0;
                data->timing_misc_ticks = 0;
                data->timing_window_start_ticks = now;
            }
        }
    }

    if (data->collect_span_stats) {
        data->geometry_stats_cache.triangles = data->stats_triangles;
        data->geometry_stats_cache.spans = data->stats_spans;
        data->geometry_stats_cache.span_pixels = data->stats_span_pixels;
        data->geometry_stats_cache.max_span_width = data->stats_max_span_width;
        data->geometry_stats_cache.max_span_height = data->stats_max_span_height;
        data->geometry_stats_valid = SDL_TRUE;

        if (data->stats_triangles > 0 && mmiyoo_debug_verbose) {
            const double triangles = (double)data->stats_triangles;
            const double spans = (double)data->stats_spans;
            const double avg_spans_per_tri = spans > 0.0 ? spans / triangles : 0.0;
            const double avg_pixels_per_span = spans > 0.0 ? (double)data->stats_span_pixels / spans : 0.0;
            const double avg_pixels_per_tri = triangles > 0.0 ? (double)data->stats_span_pixels / triangles : 0.0;
            SDL_LogDebug(SDL_LOG_CATEGORY_RENDER,
                         "MMIYOO geom stats: tri=%llu spans=%llu avgSpanPerTri=%.2f avgPixelsPerSpan=%.2f avgPixelsPerTri=%.2f maxSpan=%ux%u",
                         (unsigned long long)data->stats_triangles,
                         (unsigned long long)data->stats_spans,
                         avg_spans_per_tri,
                         avg_pixels_per_span,
                         avg_pixels_per_tri,
                         data->stats_max_span_width,
                         data->stats_max_span_height);
        }

        data->stats_triangles = 0;
        data->stats_spans = 0;
        data->stats_span_pixels = 0;
        data->stats_max_span_height = 0;
        data->stats_max_span_width = 0;
    }

}

int MMIYOO_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;

    data->vsync = (vsync != 0) ? SDL_TRUE : SDL_FALSE;
    MMIYOO_UpdatePresentVSyncFlag(renderer, data->vsync);
    return 0;
}

#endif /* SDL_VIDEO_RENDER_MMIYOO */
