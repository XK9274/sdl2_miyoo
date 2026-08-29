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

/* Viewport/target coordinate transforms, clipping, triangle rasterization,
 * QuickFill/DrawLine primitive execution, and draw-state helpers. Pure
 * primitive layer -- no knowledge of the SDL command queue. */

int
MMIYOO_FloatToPixel(float value)
{
    return (int)SDL_floorf(value + 0.5f);
}

void
MMIYOO_ApplyViewportToPoint(const MMIYOO_RenderData *data, float *x, float *y)
{
    if (data->viewport_enabled) {
        *x += data->viewport.x;
        *y += data->viewport.y;
    }
}

inline int
MMIYOO_GetFramebufferWidth(const MMIYOO_RenderData *data)
{
    if (data->framebuffer_width > 0) {
        return data->framebuffer_width;
    }
    return (int)GFX_GetFrameWidth();
}

inline int
MMIYOO_GetFramebufferHeight(const MMIYOO_RenderData *data)
{
    if (data->framebuffer_height > 0) {
        return data->framebuffer_height;
    }
    return (int)GFX_GetFrameHeight();
}

static inline void
MMIYOO_TransformToTarget(const MMIYOO_RenderData *data, float x, float y, float *out_x, float *out_y)
{
    if (data->is_target_texture) {
        *out_x = x;
        *out_y = y;
    } else {
        const int target_width = MMIYOO_GetFramebufferWidth(data);
        const int target_height = MMIYOO_GetFramebufferHeight(data);
        *out_x = (float)target_width - x - 1.0f;
        *out_y = (float)target_height - y - 1.0f;
    }
}

static int
MMIYOO_ComputeClipCode(float x, float y, float x_min, float x_max, float y_min, float y_max)
{
    const int LEFT = 1;
    const int RIGHT = 2;
    const int BOTTOM = 4;
    const int TOP = 8;
    int code = 0;

    if (x < x_min) {
        code |= LEFT;
    } else if (x > x_max) {
        code |= RIGHT;
    }

    if (y < y_min) {
        code |= TOP;
    } else if (y > y_max) {
        code |= BOTTOM;
    }

    return code;
}

SDL_bool
MMIYOO_ClipLineToRect(float *x0, float *y0, float *x1, float *y1, const SDL_Rect *rect)
{
    float x_min = (float)rect->x;
    float x_max = (float)(rect->x + rect->w - 1);
    float y_min = (float)rect->y;
    float y_max = (float)(rect->y + rect->h - 1);
    int outcode0 = MMIYOO_ComputeClipCode(*x0, *y0, x_min, x_max, y_min, y_max);
    int outcode1 = MMIYOO_ComputeClipCode(*x1, *y1, x_min, x_max, y_min, y_max);

    while (SDL_TRUE) {
        if (!(outcode0 | outcode1)) {
            return SDL_TRUE;
        } else if (outcode0 & outcode1) {
            return SDL_FALSE;
        } else {
            float x, y;
            int outcodeOut = outcode0 ? outcode0 : outcode1;

            if (outcodeOut & 8) { /* TOP */
                x = *x0 + (*x1 - *x0) * (y_min - *y0) / (*y1 - *y0);
                y = y_min;
            } else if (outcodeOut & 4) { /* BOTTOM */
                x = *x0 + (*x1 - *x0) * (y_max - *y0) / (*y1 - *y0);
                y = y_max;
            } else if (outcodeOut & 2) { /* RIGHT */
                y = *y0 + (*y1 - *y0) * (x_max - *x0) / (*x1 - *x0);
                x = x_max;
            } else { /* LEFT */
                y = *y0 + (*y1 - *y0) * (x_min - *x0) / (*x1 - *x0);
                x = x_min;
            }

            if (outcodeOut == outcode0) {
                *x0 = x;
                *y0 = y;
                outcode0 = MMIYOO_ComputeClipCode(*x0, *y0, x_min, x_max, y_min, y_max);
            } else {
                *x1 = x;
                *y1 = y;
                outcode1 = MMIYOO_ComputeClipCode(*x1, *y1, x_min, x_max, y_min, y_max);
            }
        }
    }
}

// Edge state for incremental scanline rasterization
typedef struct {
    float x_current;    // Current X intersection (for horizontal scanning)
    float y_current;    // Current Y intersection (for vertical scanning)
    float dx_dy;        // X increment per Y step (slope)
    float dy_dx;        // Y increment per X step (inverse slope)
    int y_min, y_max;   // Y range where edge is active
    int x_min, x_max;   // X range where edge is active
    SDL_bool active;    // Whether edge is active for current scanline
} MMIYOO_Edge;

// Called once per triangle edge.
static void
MMIYOO_SetupEdge(MMIYOO_Edge *edge, const SDL_FPoint *p0, const SDL_FPoint *p1)
{
    float dx = p1->x - p0->x;
    float dy = p1->y - p0->y;

    edge->active = SDL_FALSE;

    if (dy == 0.0f) {
        edge->y_min = edge->y_max = (int)SDL_floorf(p0->y);
        edge->dx_dy = 0.0f;
        edge->x_current = (p0->x < p1->x) ? p0->x : p1->x;
        edge->x_min = (int)SDL_floorf(edge->x_current);
        edge->x_max = (int)SDL_floorf((p0->x > p1->x) ? p0->x : p1->x);
        return;
    }

    if (dx == 0.0f) {
        edge->x_min = edge->x_max = (int)SDL_floorf(p0->x);
        edge->dy_dx = 0.0f;
        edge->dx_dy = 0.0f;
        edge->y_current = (p0->y < p1->y) ? p0->y : p1->y;
        edge->y_min = (int)SDL_floorf(edge->y_current);
        edge->y_max = (int)SDL_floorf((p0->y > p1->y) ? p0->y : p1->y);
        edge->x_current = p0->x;
        return;
    }

    edge->dx_dy = dx / dy;
    edge->dy_dx = dy / dx;

    if (p0->y <= p1->y) {
        edge->y_min = (int)SDL_floorf(p0->y);
        edge->y_max = (int)SDL_floorf(p1->y);
        edge->x_current = p0->x;
    } else {
        edge->y_min = (int)SDL_floorf(p1->y);
        edge->y_max = (int)SDL_floorf(p0->y);
        edge->x_current = p1->x;
    }

    if (p0->x <= p1->x) {
        edge->x_min = (int)SDL_floorf(p0->x);
        edge->x_max = (int)SDL_floorf(p1->x);
        edge->y_current = p0->y;
    } else {
        edge->x_min = (int)SDL_floorf(p1->x);
        edge->x_max = (int)SDL_floorf(p0->x);
        edge->y_current = p1->y;
    }
}

// Triangle filling via incremental edge walking.

void
MMIYOO_DrawFilledTriangle(MMIYOO_RenderData *data,
                                   const SDL_FPoint *p0,
                                   const SDL_FPoint *p1,
                                   const SDL_FPoint *p2,
                                   const SDL_Rect *clip_rect,
                                   Uint32 color)
{
    MMIYOO_Edge edges[3];
    SDL_Rect *span_buffer;
    Uint32 triangle_span_count = 0;
    Uint64 triangle_span_pixels = 0;
    Uint32 triangle_max_span_height = 0;
    Uint32 triangle_max_span_width = 0;
    float tri_min_y;
    float tri_max_y;
    int y_start;
    int y_end;
    int y;
    int span_capacity;
    int span_count;
    int band_limit;
    int i;
    const int edge_tolerance = 1;

    MMIYOO_SetupEdge(&edges[0], p0, p1);
    MMIYOO_SetupEdge(&edges[1], p1, p2);
    MMIYOO_SetupEdge(&edges[2], p2, p0);

    tri_min_y = p0->y;
    if (p1->y < tri_min_y) tri_min_y = p1->y;
    if (p2->y < tri_min_y) tri_min_y = p2->y;

    tri_max_y = p0->y;
    if (p1->y > tri_max_y) tri_max_y = p1->y;
    if (p2->y > tri_max_y) tri_max_y = p2->y;

    y_start = (int)SDL_floorf(tri_min_y);
    y_end = (int)SDL_floorf(tri_max_y);

    if (clip_rect) {
        int clip_y_start = clip_rect->y;
        int clip_y_end = clip_rect->y + clip_rect->h - 1;
        if (y_start < clip_y_start) {
            y_start = clip_y_start;
        }
        if (y_end > clip_y_end) {
            y_end = clip_y_end;
        }
    }

    if (y_start > y_end) {
        return;
    }

    band_limit = SDL_max(1, (int)data->span_band_height);

    span_capacity = y_end - y_start + 1;
    span_buffer = (SDL_Rect *)SDL_stack_alloc(SDL_Rect, span_capacity);
    if (!span_buffer) {
        return;
    }
    span_count = 0;

    for (i = 0; i < 3; i++) {
        if (edges[i].y_min >= 0 && y_start >= edges[i].y_min && y_start <= edges[i].y_max) {
            if (edges[i].dx_dy != 0.0f) {
                float dy_offset = (float)y_start + 0.5f - (float)edges[i].y_min;
                edges[i].x_current += edges[i].dx_dy * dy_offset;
            }
            edges[i].active = SDL_TRUE;
        }
    }

    for (y = y_start; y <= y_end; ++y) {
        float scan_y = (float)y + 0.5f;
        float xs[3];
        float min_x;
        float max_x;
        int span_x_start;
        int span_x_end;
        int count = 0;

        for (i = 0; i < 3; i++) {
            if (edges[i].y_min >= 0 &&
                scan_y >= (float)edges[i].y_min &&
                scan_y <= (float)edges[i].y_max) {
                xs[count++] = edges[i].x_current;
                if (edges[i].dx_dy != 0.0f) {
                    edges[i].x_current += edges[i].dx_dy;
                }
            }
        }

        if (count < 2) {
            continue;
        }

        min_x = max_x = xs[0];
        for (i = 1; i < count; i++) {
            if (xs[i] < min_x) {
                min_x = xs[i];
            }
            if (xs[i] > max_x) {
                max_x = xs[i];
            }
        }

        span_x_start = (int)SDL_floorf(min_x);
        span_x_end = (int)SDL_ceilf(max_x) - 1;

        if (clip_rect) {
            int clip_x_start = clip_rect->x;
            int clip_x_end = clip_rect->x + clip_rect->w - 1;
            if (span_x_start < clip_x_start) {
                span_x_start = clip_x_start;
            }
            if (span_x_end > clip_x_end) {
                span_x_end = clip_x_end;
            }
        }

        if (span_x_end < span_x_start) {
            continue;
        }

        span_buffer[span_count].x = span_x_start;
        span_buffer[span_count].y = y;
        span_buffer[span_count].w = span_x_end - span_x_start + 1;
        span_buffer[span_count].h = 1;
        span_count++;
    }

    if (span_count == 0) {
        SDL_stack_free(span_buffer);
        return;
    }

    for (i = 0; i < span_count; ) {
        SDL_Rect merged = span_buffer[i];
        int band_height = 1;

        while (band_height < band_limit && (i + band_height) < span_count) {
            SDL_Rect next = span_buffer[i + band_height];
            int left_delta;
            int right_delta;
            if (next.y != merged.y + band_height) {
                break;
            }

            left_delta = SDL_abs(next.x - merged.x);
            right_delta = SDL_abs((next.x + next.w) - (merged.x + merged.w));
            if (left_delta > edge_tolerance || right_delta > edge_tolerance) {
                break;
            }
            {
                int new_left = SDL_min(merged.x, next.x);
                int new_right = SDL_max(merged.x + merged.w, next.x + next.w);
                merged.x = new_left;
                merged.w = new_right - new_left;
            }
            merged.h += 1;
            band_height++;
        }

        if (data->collect_span_stats) {
            triangle_span_count++;
            triangle_span_pixels += (Uint64)merged.w * (Uint64)merged.h;
            if ((Uint32)merged.h > triangle_max_span_height) {
                triangle_max_span_height = (Uint32)merged.h;
            }
            if ((Uint32)merged.w > triangle_max_span_width) {
                triangle_max_span_width = (Uint32)merged.w;
            }
        }

        MMIYOO_ExecuteQuickFill(data, &merged, color);
        i += band_height;
    }

    SDL_stack_free(span_buffer);

    if (data->collect_span_stats && triangle_span_count > 0) {
        data->stats_triangles += 1;
        data->stats_spans += triangle_span_count;
        data->stats_span_pixels += triangle_span_pixels;
        if (triangle_max_span_height > data->stats_max_span_height) {
            data->stats_max_span_height = triangle_max_span_height;
        }
        if (triangle_max_span_width > data->stats_max_span_width) {
            data->stats_max_span_width = triangle_max_span_width;
        }
    }
}

Uint32
MMIYOO_PackColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    return (((Uint32)a) << 24) | (((Uint32)r) << 16) | (((Uint32)g) << 8) | (Uint32)b;
}

void
MMIYOO_ExecuteQuickFill(MMIYOO_RenderData *data, const SDL_Rect *dst, Uint32 color)
{
    MI_GFX_Rect_t dst_rect;
    MI_U16 fence = 0;
    MI_S32 result;

    if (!dst || dst->w <= 0 || dst->h <= 0) {
        return;
    }

    /* Apply coordinate flipping for framebuffer targets (physically upside-down display) */
    if (data->is_target_texture) {
        dst_rect.s32Xpos = dst->x;
        dst_rect.s32Ypos = dst->y;
    } else {
        const int framebuffer_width = MMIYOO_GetFramebufferWidth(data);
        const int framebuffer_height = MMIYOO_GetFramebufferHeight(data);
        dst_rect.s32Xpos = framebuffer_width - dst->x - dst->w;
        dst_rect.s32Ypos = framebuffer_height - dst->y - dst->h;
    }
    dst_rect.u32Width = dst->w;
    dst_rect.u32Height = dst->h;

    result = MI_GFX_QuickFill(&data->current_target_surface, &dst_rect, color, &fence);
    if (result == MI_SUCCESS) {
        GFX_AddTextureFence(fence);
    } else {
        MMIYOO_LOG_WARN("QuickFill: MI_GFX_QuickFill failed (result=%d)", result);
    }
}

SDL_bool
MMIYOO_ExecuteDrawLine(MMIYOO_RenderData *data,
                       float x0,
                       float y0,
                       float x1,
                       float y1,
                       Uint32 color)
{
    MI_GFX_Line_t line;
    MI_U16 fence = 0;
    MI_S32 result;
    SDL_Rect bounds = MMIYOO_GetTargetBounds(data);
    float hw_x0, hw_y0, hw_x1, hw_y1;
    int ix0, iy0, ix1, iy1;
    int min_x, max_x, min_y, max_y;

    if (bounds.w <= 0 || bounds.h <= 0) {
        return SDL_FALSE;
    }

    MMIYOO_TransformToTarget(data, x0, y0, &hw_x0, &hw_y0);
    MMIYOO_TransformToTarget(data, x1, y1, &hw_x1, &hw_y1);

    ix0 = MMIYOO_FloatToPixel(hw_x0);
    iy0 = MMIYOO_FloatToPixel(hw_y0);
    ix1 = MMIYOO_FloatToPixel(hw_x1);
    iy1 = MMIYOO_FloatToPixel(hw_y1);

    min_x = bounds.x;
    max_x = bounds.x + bounds.w - 1;
    min_y = bounds.y;
    max_y = bounds.y + bounds.h - 1;

    if (ix0 < min_x) ix0 = min_x;
    if (ix0 > max_x) ix0 = max_x;
    if (iy0 < min_y) iy0 = min_y;
    if (iy0 > max_y) iy0 = max_y;
    if (ix1 < min_x) ix1 = min_x;
    if (ix1 > max_x) ix1 = max_x;
    if (iy1 < min_y) iy1 = min_y;
    if (iy1 > max_y) iy1 = max_y;

    SDL_zero(line);
    line.stPointFrom.s16x = (MI_S16)ix0;
    line.stPointFrom.s16y = (MI_S16)iy0;
    line.stPointTo.s16x = (MI_S16)ix1;
    line.stPointTo.s16y = (MI_S16)iy1;

    {
        MI_S16 tmp_x;
        MI_S16 tmp_y;
        const int dx_initial = SDL_abs((int)line.stPointTo.s16x - (int)line.stPointFrom.s16x);
        const int dy_initial = SDL_abs((int)line.stPointTo.s16y - (int)line.stPointFrom.s16y);

        if (dy_initial > dx_initial) {
            if (line.stPointFrom.s16y > line.stPointTo.s16y) {
                tmp_x = line.stPointFrom.s16x;
                tmp_y = line.stPointFrom.s16y;
                line.stPointFrom.s16x = line.stPointTo.s16x;
                line.stPointFrom.s16y = line.stPointTo.s16y;
                line.stPointTo.s16x = tmp_x;
                line.stPointTo.s16y = tmp_y;
            }
        } else {
            if (line.stPointFrom.s16x > line.stPointTo.s16x) {
                tmp_x = line.stPointFrom.s16x;
                tmp_y = line.stPointFrom.s16y;
                line.stPointFrom.s16x = line.stPointTo.s16x;
                line.stPointFrom.s16y = line.stPointTo.s16y;
                line.stPointTo.s16x = tmp_x;
                line.stPointTo.s16y = tmp_y;
            }
        }

        {
            const int dx = SDL_abs((int)line.stPointTo.s16x - (int)line.stPointFrom.s16x);
            const int dy = SDL_abs((int)line.stPointTo.s16y - (int)line.stPointFrom.s16y);

            if (dx == 0 && dy == 0) {
                return SDL_TRUE;
            }

            if (dy > dx) {
                if (line.stPointTo.s16y < max_y) {
                    line.stPointTo.s16y += 1;
                } else if (line.stPointFrom.s16y > min_y) {
                    line.stPointFrom.s16y -= 1;
                }
            } else {
                if (line.stPointTo.s16x < max_x) {
                    line.stPointTo.s16x += 1;
                } else if (line.stPointFrom.s16x > min_x) {
                    line.stPointFrom.s16x -= 1;
                }
            }
        }
    }
    line.u16Width = 1;
    line.bColorGradient = FALSE;
    line.u32ColorFrom = color;
    line.u32ColorTo = color;

    result = MI_GFX_DrawLine(&data->current_target_surface, &line, &fence);
    if (result == MI_SUCCESS) {
        GFX_AddTextureFence(fence);
        return SDL_TRUE;
    }

    MMIYOO_LOG_WARN("DrawLine: MI_GFX_DrawLine failed (result=%d)", result);
    return SDL_FALSE;
}

SDL_Rect
MMIYOO_GetTargetBounds(const MMIYOO_RenderData *data)
{
    SDL_Rect bounds;
    bounds.x = 0;
    bounds.y = 0;
    if (data->is_target_texture) {
        bounds.w = (int)data->current_target_surface.u32Width;
        bounds.h = (int)data->current_target_surface.u32Height;
    } else {
        bounds.w = MMIYOO_GetFramebufferWidth(data);
        bounds.h = MMIYOO_GetFramebufferHeight(data);
    }
    return bounds;
}

SDL_bool
MMIYOO_PrepareDrawRect(SDL_Renderer *renderer,
                       MMIYOO_RenderData *data,
                       SDL_Rect *dst,
                       SDL_Rect *src,
                       SDL_Rect *out_clip,
                       SDL_bool *clip_enabled)
{
    SDL_Rect target_bounds = MMIYOO_GetTargetBounds(data);
    SDL_Rect base_clip = target_bounds;
    SDL_Rect original_dst;
    SDL_Rect final_dst;
    SDL_Rect final_clip;

    if (!dst || dst->w <= 0 || dst->h <= 0) {
        return SDL_FALSE;
    }

    original_dst = *dst;

    if (data->viewport_enabled) {
        original_dst.x += data->viewport.x;
        original_dst.y += data->viewport.y;
        base_clip = data->viewport;

        /* Viewport can exceed the real target bounds; never clip past them. */
        if (!SDL_IntersectRect(&base_clip, &target_bounds, &base_clip)) {
            return SDL_FALSE;
        }
    }

    if (!SDL_IntersectRect(&original_dst, &base_clip, &final_dst)) {
        return SDL_FALSE;
    }

    final_clip = base_clip;

    if (data->clip_enabled || renderer->clipping_enabled) {
        SDL_Rect clip_rect;
        SDL_bool apply_clip = SDL_FALSE;

        if (data->clip_enabled) {
            clip_rect = data->clip_rect;
            apply_clip = SDL_TRUE;
        } else if (renderer->clipping_enabled) {
            clip_rect.x = (int)SDL_floorf(renderer->clip_rect.x);
            clip_rect.y = (int)SDL_floorf(renderer->clip_rect.y);
            clip_rect.w = (int)SDL_floorf(renderer->clip_rect.w);
            clip_rect.h = (int)SDL_floorf(renderer->clip_rect.h);
            apply_clip = SDL_TRUE;
        }

        if (apply_clip) {
            if (clip_rect.w < 0) {
                clip_rect.w = 0;
            }
            if (clip_rect.h < 0) {
                clip_rect.h = 0;
            }

            clip_rect.x += base_clip.x;
            clip_rect.y += base_clip.y;

            if (!SDL_IntersectRect(&final_clip, &clip_rect, &final_clip)) {
                return SDL_FALSE;
            }

            if (!SDL_IntersectRect(&final_dst, &final_clip, &final_dst)) {
                return SDL_FALSE;
            }
        }
    }

    if (src) {
        const int original_src_w = src->w;
        const int original_src_h = src->h;
        const int original_dst_right = original_dst.x + original_dst.w;
        const int original_dst_bottom = original_dst.y + original_dst.h;
        float inv_scale_x;
        float inv_scale_y;

        if (original_src_w <= 0 || original_src_h <= 0 || original_dst.w <= 0 || original_dst.h <= 0) {
            return SDL_FALSE;
        }

        inv_scale_x = (float)original_src_w / (float)original_dst.w;
        inv_scale_y = (float)original_src_h / (float)original_dst.h;

        if (final_dst.x > original_dst.x) {
            int delta_left = final_dst.x - original_dst.x;
            int src_delta = (int)SDL_floorf(delta_left * inv_scale_x);
            src->x += src_delta;
            src->w -= src_delta;
        }

        if ((final_dst.x + final_dst.w) < original_dst_right) {
            int delta_right = original_dst_right - (final_dst.x + final_dst.w);
            int src_delta = (int)SDL_floorf(delta_right * inv_scale_x);
            src->w -= src_delta;
        }

        if (final_dst.y > original_dst.y) {
            int delta_top = final_dst.y - original_dst.y;
            int src_delta = (int)SDL_floorf(delta_top * inv_scale_y);
            src->y += src_delta;
            src->h -= src_delta;
        }

        if ((final_dst.y + final_dst.h) < original_dst_bottom) {
            int delta_bottom = original_dst_bottom - (final_dst.y + final_dst.h);
            int src_delta = (int)SDL_floorf(delta_bottom * inv_scale_y);
            src->h -= src_delta;
        }

        if (src->w <= 0 || src->h <= 0) {
            return SDL_FALSE;
        }
    }

    if (!data->is_target_texture && SDL_GetHintBoolean("SDL_MMIYOO_DEBUG_LOG", SDL_FALSE)) {
        MMIYOO_LOG_WARN("SCALEDBG PrepareDrawRect: target_bounds=(%d,%d,%d,%d) viewport_on=%d viewport=(%d,%d,%d,%d) original_dst=(%d,%d,%d,%d) final_dst=(%d,%d,%d,%d)",
                        target_bounds.x, target_bounds.y, target_bounds.w, target_bounds.h,
                        (int)data->viewport_enabled,
                        data->viewport.x, data->viewport.y, data->viewport.w, data->viewport.h,
                        original_dst.x, original_dst.y, original_dst.w, original_dst.h,
                        final_dst.x, final_dst.y, final_dst.w, final_dst.h);
    }

    *dst = final_dst;

    if (out_clip) {
        *out_clip = final_clip;
    }
    if (clip_enabled) {
        *clip_enabled = (renderer->clipping_enabled == SDL_TRUE) || data->viewport_enabled;
    }

    return SDL_TRUE;
}

#endif /* SDL_VIDEO_RENDER_MMIYOO */
