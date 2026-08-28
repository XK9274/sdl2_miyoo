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

#define MMIYOO_SYS_ALIGNMENT 4096u
#define MMIYOO_ALIGN_SYS(value) (((value) + MMIYOO_SYS_ALIGNMENT - 1u) & ~(MMIYOO_SYS_ALIGNMENT - 1u))


typedef struct MMIYOO_RenderData MMIYOO_RenderData;

static SDL_bool g_warned_copyex_angle = SDL_FALSE;

static SDL_bool mmiyoo_debug_verbose = SDL_FALSE;
static int mmiyoo_texture_live_count = 0;

static void
MMIYOO_FlushInvCacheRange(void *address, size_t size)
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t aligned_start;
    uintptr_t aligned_end;

    if (!address || !size) {
        return;
    }

    start = (uintptr_t)address;
    end = start + size;
    aligned_start = start & ~(uintptr_t)(MMIYOO_SYS_ALIGNMENT - 1u);
    aligned_end = (end + MMIYOO_SYS_ALIGNMENT - 1u) &
                  ~(uintptr_t)(MMIYOO_SYS_ALIGNMENT - 1u);
    MI_SYS_FlushInvCache((void *)aligned_start,
                         (MI_U32)(aligned_end - aligned_start));
}

#define MMIYOO_VERBOSE_LOG(fmt, ...)                          \
    do {                                                      \
        if (mmiyoo_debug_verbose) {                           \
            MMIYOO_LOG_DEBUG(fmt, ##__VA_ARGS__);             \
        }                                                     \
    } while (0)

typedef struct MMIYOO_TextureData {
    void *data;
    unsigned int size;
    unsigned int width;
    unsigned int height;
    unsigned int bits;
    unsigned int format;
    unsigned int pitch;
    MI_PHY phyAddr;
    void *virAddr;
    SDL_bool uses_msys_memory;
    MI_GFX_Surface_t gfx_surface;
    MI_GFX_ColorFmt_e mi_format;
    Uint32 bytes_per_pixel;
    /* Actual MI_SYS_MMA_Alloc/Mmap size backing this texture -- may be >=
     * size when this texture reused a pooled block. MI_SYS_Munmap must
     * always be called with this, never with the (possibly smaller)
     * logical size above. */
    unsigned int alloc_size;
} MMIYOO_TextureData;

/* Bounded, size-bucketed reuse pool for MI_SYS_MMA texture memory blocks.
 * The MMA heap is a small, fixed reserved region (confirmed ~20.75MB via
 * /proc/cmdline's mma_heap=...,sz=0x1500000) shared with the rest of the
 * system; every MMIYOO_CreateTexture/DestroyTexture pair was previously a
 * fresh MI_SYS_MMA_Alloc/Free round-trip with zero reuse, a classic
 * fragmentation pattern for a small reserved-memory allocator. Freed
 * blocks are cached here instead of freed immediately, and reused by a
 * later CreateTexture of a compatible size. Strictly a release valve: hard
 * count/byte caps mean it can never itself become a new exhaustion source
 * -- worst case (caps hit, or disabled via hint) behaves exactly like no
 * pool at all. GFX_FlushTextureFences() (already called unconditionally in
 * MMIYOO_DestroyTexture before a block reaches this pool) is a *global*
 * fence drain, so a cached block is always already safe to reuse -- no
 * additional per-block synchronization is needed. */
typedef struct {
    MI_PHY phyAddr;
    void *virAddr;
    unsigned int alloc_size;
} MMIYOO_PooledBlock;

#define MMIYOO_TEXTURE_POOL_MAX_ENTRIES 24
#define MMIYOO_TEXTURE_POOL_DEFAULT_MAX_BYTES (10u * 1024u * 1024u)
/* Reject a candidate block if satisfying the request would waste more than
 * this fraction of it, so the pool can't permanently pin oversized blocks
 * against tiny requests. */
#define MMIYOO_TEXTURE_POOL_SLACK_MUL 2

static SDL_SpinLock mmiyoo_texture_pool_lock = 0;
static MMIYOO_PooledBlock mmiyoo_texture_pool[MMIYOO_TEXTURE_POOL_MAX_ENTRIES];
static int mmiyoo_texture_pool_count = 0;
static unsigned int mmiyoo_texture_pool_bytes = 0;
static unsigned int mmiyoo_texture_pool_max_bytes = MMIYOO_TEXTURE_POOL_DEFAULT_MAX_BYTES;
static SDL_bool mmiyoo_texture_pool_enabled = SDL_TRUE;

static unsigned int mmiyoo_pool_hits = 0;
static unsigned int mmiyoo_pool_misses = 0;
static unsigned int mmiyoo_pool_evictions = 0;
static unsigned int mmiyoo_pool_drains = 0;

struct MMIYOO_RenderData {
    SDL_Texture *boundTarget;
    SDL_bool initialized;
    unsigned int bpp;
    SDL_bool vsync;
    MI_GFX_Surface_t current_target_surface;
    SDL_bool is_target_texture;
    SDL_Rect viewport;
    SDL_bool viewport_enabled;
    SDL_bool clip_enabled;
    SDL_Rect clip_rect;

    // Color state for draw operations
    Uint8 draw_color_r;
    Uint8 draw_color_g;
    Uint8 draw_color_b;
    Uint8 draw_color_a;

    // Track if texture was blitted to framebuffer this frame
    SDL_bool texture_blitted_to_screen;

    // Optional geometry instrumentation (enabled via SDL_MMIYOO_GEOMETRY_STATS hint)
    SDL_bool collect_span_stats;
    Uint64 stats_triangles;
    Uint64 stats_spans;
    Uint64 stats_span_pixels;
    Uint32 stats_max_span_height;
    Uint32 stats_max_span_width;
    Uint8 span_band_height;

    int framebuffer_width;
    int framebuffer_height;

    // Optional per-frame timing instrumentation (SDL_MMIYOO_FRAME_TIMING hint)
    // -- finds where frame time actually goes (command-queue processing vs.
    // present/swap) instead of guessing from blit-call counts alone.
    SDL_bool collect_frame_timing;
    Uint64 timing_command_queue_ticks;
    Uint64 timing_present_ticks;
    Uint64 timing_blit_calls;
    Uint64 timing_frames;
    Uint64 timing_window_start_ticks;

    /* cmdQueue breakdown by SDL_RenderCommand category, so the aggregate
     * cmdQueue number can be attributed to fills/QuickFill, copies/blits,
     * textured geometry, lines, or trivial state-setting commands, instead
     * of guessing from blit-call counts alone. */
    Uint64 timing_fill_ticks;
    Uint64 timing_copy_ticks;
    Uint64 timing_geometry_ticks;
    Uint64 timing_lines_ticks;
    Uint64 timing_misc_ticks;

    /* SDL_MMIYOO_GEOMETRY_QUICKPATH hint: collapse a glyph quad's two
     * identical-rect triangles into one blit instead of two. Off by
     * default -- trades a little text crispness (drops the accidental
     * double alpha-composite) for fewer hardware blits. */
    SDL_bool geometry_quickpath_enabled;

    /* SDL_MMIYOO_INTEGER_SCALE hint (on by default): software NEON upscale before an unscaled hardware present, since MI_GFX_BitBlit has no interpolation control. */
    SDL_bool integer_scale_enabled;
    MI_PHY scale_scratch_phy;
    void *scale_scratch_vir;
    unsigned int scale_scratch_alloc_size;
    /* Latched after a failed grow attempt so a sustained MMA-exhaustion condition doesn't retry every frame; cleared on the next successful grow. */
    SDL_bool scale_scratch_alloc_failed;

    /* Latched after MMIYOO_TryDownscaleCompositeCopy first hits a degenerate/
     * unsupported case (zero-size texture, non-32bpp format) so it logs once
     * instead of every frame. */
    SDL_bool downscale_unsupported_warned;
};

typedef struct {
    SDL_Rect rect;
    SDL_bool vertical;
    SDL_bool active;
} MMIYOO_LineBatch;

typedef struct {
    SDL_Rect rect;
    SDL_bool horizontal;
    SDL_bool active;
} MMIYOO_RectBatch;

typedef struct {
    float x;
    float y;
    SDL_Color color;
} MMIYOO_GeometryFillVertex;

typedef struct {
    SDL_Rect srcrect;
    SDL_FRect dstrect;
    SDL_bool skip; /* SDL_MMIYOO_GEOMETRY_QUICKPATH: duplicate of the previous triangle, drop its blit */
} MMIYOO_GeometryTextureTri;

/* One bounding-box hardware blit per triangle, not one for the whole
 * SDL_RenderGeometry call -- a single call batches many disjoint quads
 * (e.g. every glyph in a text line, each sampling its own atlas
 * subrect), so a single whole-batch bounding box stretched a random
 * crop of unrelated glyphs across the entire line. tri_count
 * MMIYOO_GeometryTextureTri entries follow this header in the
 * allocated vertex buffer. */
typedef struct {
    MI_GFX_Surface_t target_surface;
    SDL_bool is_target_texture;
    int tri_count;
} MMIYOO_GeometryTextureData;

static void MMIYOO_ExecuteQuickFill(MMIYOO_RenderData *data, const SDL_Rect *dst, Uint32 color);
static SDL_Rect MMIYOO_GetTargetBounds(const MMIYOO_RenderData *data);
static SDL_bool MMIYOO_ExecuteDrawLine(MMIYOO_RenderData *data,
                                       float x0,
                                       float y0,
                                       float x1,
                                       float y1,
                                       Uint32 color);
static void MMIYOO_LineBatchFlush(SDL_Renderer *renderer, MMIYOO_RenderData *data, MMIYOO_LineBatch *batch, Uint32 color);
static void MMIYOO_RectBatchFlush(SDL_Renderer *renderer, MMIYOO_RenderData *data, MMIYOO_RectBatch *batch, Uint32 color);
static void MMIYOO_ProcessFillCommand(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices);
static void MMIYOO_ProcessDrawLines(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices);
static void MMIYOO_ProcessGeometry(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices);

extern GFX gfx;
extern SDL_Surface *fps_info;
extern int show_fps;
extern int down_scale;

int need_reload_bg = 0;

static inline int
MMIYOO_FloatToPixel(float value)
{
    return (int)SDL_floorf(value + 0.5f);
}

static void
MMIYOO_ApplyViewportToPoint(const MMIYOO_RenderData *data, float *x, float *y)
{
    if (data->viewport_enabled) {
        *x += data->viewport.x;
        *y += data->viewport.y;
    }
}

static inline int
MMIYOO_GetFramebufferWidth(const MMIYOO_RenderData *data)
{
    if (data->framebuffer_width > 0) {
        return data->framebuffer_width;
    }
    return (int)GFX_GetFrameWidth();
}

static inline int
MMIYOO_GetFramebufferHeight(const MMIYOO_RenderData *data)
{
    if (data->framebuffer_height > 0) {
        return data->framebuffer_height;
    }
    return (int)GFX_GetFrameHeight();
}

static inline int
MMIYOO_GetCurrentTargetWidth(const MMIYOO_RenderData *data)
{
    if (data->is_target_texture) {
        if (data->current_target_surface.u32Width > 0) {
            return (int)data->current_target_surface.u32Width;
        }
        if (data->boundTarget) {
            return data->boundTarget->w;
        }
    }
    if (data->current_target_surface.u32Width > 0) {
        return (int)data->current_target_surface.u32Width;
    }
    return MMIYOO_GetFramebufferWidth(data);
}

static inline int
MMIYOO_GetCurrentTargetHeight(const MMIYOO_RenderData *data)
{
    if (data->is_target_texture) {
        if (data->current_target_surface.u32Height > 0) {
            return (int)data->current_target_surface.u32Height;
        }
        if (data->boundTarget) {
            return data->boundTarget->h;
        }
    }
    if (data->current_target_surface.u32Height > 0) {
        return (int)data->current_target_surface.u32Height;
    }
    return MMIYOO_GetFramebufferHeight(data);
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

static SDL_bool
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

// Optimized edge structure for incremental rasterization
typedef struct {
    float x_current;    // Current X intersection (for horizontal scanning)
    float y_current;    // Current Y intersection (for vertical scanning)
    float dx_dy;        // X increment per Y step (slope)
    float dy_dx;        // Y increment per X step (inverse slope)
    int y_min, y_max;   // Y range where edge is active
    int x_min, x_max;   // X range where edge is active
    SDL_bool active;    // Whether edge is active for current scanline
} MMIYOO_Edge;

// Setup edge data for incremental walking - called once per triangle
static void
MMIYOO_SetupEdge(MMIYOO_Edge *edge, const SDL_FPoint *p0, const SDL_FPoint *p1)
{
    float dx = p1->x - p0->x;
    float dy = p1->y - p0->y;

    edge->active = SDL_FALSE;

    // Handle horizontal edges (still need Y range for boundary checking)
    if (dy == 0.0f) {
        edge->y_min = edge->y_max = (int)SDL_floorf(p0->y);
        edge->dx_dy = 0.0f;
        edge->x_current = (p0->x < p1->x) ? p0->x : p1->x;
        edge->x_min = (int)SDL_floorf(edge->x_current);
        edge->x_max = (int)SDL_floorf((p0->x > p1->x) ? p0->x : p1->x);
        return;
    }

    // Handle vertical edges (still need X range for boundary checking)
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

    // Calculate slopes once - this is where we save all the performance!
    edge->dx_dy = dx / dy;
    edge->dy_dx = dy / dx;

    // Set Y range properly - always use the correct ordering
    if (p0->y <= p1->y) {
        edge->y_min = (int)SDL_floorf(p0->y);
        edge->y_max = (int)SDL_floorf(p1->y);
        edge->x_current = p0->x;
    } else {
        edge->y_min = (int)SDL_floorf(p1->y);
        edge->y_max = (int)SDL_floorf(p0->y);
        edge->x_current = p1->x;
    }

    // Set X range for boundary checking
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

// Optimized triangle filling using incremental edge walking
static void
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

static void
MMIYOO_LineBatchReset(MMIYOO_LineBatch *batch)
{
    batch->active = SDL_FALSE;
    batch->vertical = SDL_FALSE;
    SDL_zero(batch->rect);
}

static void
MMIYOO_LineBatchAccumulate(SDL_Renderer *renderer,
                           MMIYOO_RenderData *data,
                           MMIYOO_LineBatch *batch,
                           const SDL_Rect *rect,
                           SDL_bool vertical,
                           Uint32 color)
{
    if (!batch->active) {
        batch->rect = *rect;
        batch->vertical = vertical;
        batch->active = SDL_TRUE;
        return;
    }

    if (batch->vertical != vertical) {
        MMIYOO_LineBatchFlush(renderer, data, batch, color);
        batch->rect = *rect;
        batch->vertical = vertical;
        batch->active = SDL_TRUE;
        return;
    }

    if (vertical) {
        if (rect->x != batch->rect.x) {
            MMIYOO_LineBatchFlush(renderer, data, batch, color);
            batch->rect = *rect;
            batch->vertical = vertical;
            batch->active = SDL_TRUE;
            return;
        }
        {
            int top = SDL_min(batch->rect.y, rect->y);
            int bottom = SDL_max(batch->rect.y + batch->rect.h, rect->y + rect->h);
            batch->rect.y = top;
            batch->rect.h = bottom - top;
        }
    } else {
        if (rect->y != batch->rect.y) {
            MMIYOO_LineBatchFlush(renderer, data, batch, color);
            batch->rect = *rect;
            batch->vertical = vertical;
            batch->active = SDL_TRUE;
            return;
        }
        {
            int left = SDL_min(batch->rect.x, rect->x);
            int right = SDL_max(batch->rect.x + batch->rect.w, rect->x + rect->w);
            batch->rect.x = left;
            batch->rect.w = right - left;
        }
    }
}

static void
MMIYOO_LineBatchFlush(SDL_Renderer *renderer, MMIYOO_RenderData *data, MMIYOO_LineBatch *batch, Uint32 color)
{
    (void)renderer;
    if (!batch->active) {
        return;
    }
    MMIYOO_ExecuteQuickFill(data, &batch->rect, color);
    batch->active = SDL_FALSE;
}

static void
MMIYOO_RectBatchReset(MMIYOO_RectBatch *batch)
{
    batch->active = SDL_FALSE;
    batch->horizontal = SDL_TRUE;
    SDL_zero(batch->rect);
}

static void
MMIYOO_RectBatchFlush(SDL_Renderer *renderer, MMIYOO_RenderData *data, MMIYOO_RectBatch *batch, Uint32 color)
{
    (void)renderer;
    if (!batch->active) {
        return;
    }
    MMIYOO_ExecuteQuickFill(data, &batch->rect, color);
    batch->active = SDL_FALSE;
}

static void
MMIYOO_RectBatchAccumulate(SDL_Renderer *renderer,
                           MMIYOO_RenderData *data,
                           MMIYOO_RectBatch *batch,
                           const SDL_Rect *rect,
                           Uint32 color)
{
    if (!batch->active) {
        batch->rect = *rect;
        batch->horizontal = (rect->w >= rect->h);
        batch->active = SDL_TRUE;
        return;
    }

    if (batch->horizontal) {
        if (rect->y == batch->rect.y && rect->h == batch->rect.h &&
            rect->x <= batch->rect.x + batch->rect.w &&
            rect->x + rect->w >= batch->rect.x) {
            int left = SDL_min(batch->rect.x, rect->x);
            int right = SDL_max(batch->rect.x + batch->rect.w, rect->x + rect->w);
            batch->rect.x = left;
            batch->rect.w = right - left;
            return;
        }
    } else {
        if (rect->x == batch->rect.x && rect->w == batch->rect.w &&
            rect->y <= batch->rect.y + batch->rect.h &&
            rect->y + rect->h >= batch->rect.y) {
            int top = SDL_min(batch->rect.y, rect->y);
            int bottom = SDL_max(batch->rect.y + batch->rect.h, rect->y + rect->h);
            batch->rect.y = top;
            batch->rect.h = bottom - top;
            return;
        }
    }

    MMIYOO_RectBatchFlush(renderer, data, batch, color);
    batch->rect = *rect;
    batch->horizontal = (rect->w >= rect->h);
    batch->active = SDL_TRUE;
}

static Uint32
MMIYOO_PackColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    return (((Uint32)a) << 24) | (((Uint32)r) << 16) | (((Uint32)g) << 8) | (Uint32)b;
}

static void
MMIYOO_UpdateClipState(MMIYOO_RenderData *data, SDL_bool enabled, const SDL_Rect *rect)
{
    data->clip_enabled = enabled;
    if (enabled && rect) {
        data->clip_rect = *rect;
    } else {
        SDL_zero(data->clip_rect);
    }
}

static void
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

static SDL_bool
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

// Convert SDL pixel format to MI_SYS frame format
static MI_SYS_PixelFormat_e sdl_to_mi_sys_format(Uint32 sdl_format) {
    switch(sdl_format) {
        case SDL_PIXELFORMAT_RGB565:
        case SDL_PIXELFORMAT_BGR565:
            return E_MI_SYS_PIXEL_FRAME_RGB565;
            
        case SDL_PIXELFORMAT_ARGB8888:
        case SDL_PIXELFORMAT_RGBA8888:
        case SDL_PIXELFORMAT_ABGR8888:
        case SDL_PIXELFORMAT_BGRA8888:
            return E_MI_SYS_PIXEL_FRAME_ARGB8888;
            
        case SDL_PIXELFORMAT_ARGB1555:
            return E_MI_SYS_PIXEL_FRAME_ARGB1555;
            
        case SDL_PIXELFORMAT_ARGB4444:
        case SDL_PIXELFORMAT_RGBA4444:
            return E_MI_SYS_PIXEL_FRAME_ARGB4444;
            
        default:
            return E_MI_SYS_PIXEL_FRAME_ARGB8888;
    }
}

// Advanced DMA operations using MI_SYS_BufBlitPa for MI_SYS-to-MI_SYS transfers
static int texture_to_framedata(MMIYOO_TextureData *texture, MI_SYS_FrameData_t *frame_data) {
    MI_SYS_PixelFormat_e sys_format;
    
    if (!texture || !frame_data) return -1;
    
    sys_format = sdl_to_mi_sys_format(texture->format);
    
    memset(frame_data, 0, sizeof(MI_SYS_FrameData_t));
    frame_data->ePixelFormat = sys_format;
    frame_data->u16Width = texture->width;
    frame_data->u16Height = texture->height;
    frame_data->u32Stride[0] = texture->pitch;
    frame_data->phyAddr[0] = texture->phyAddr;
    frame_data->pVirAddr[0] = texture->virAddr;
    frame_data->u32BufSize = texture->size;
    frame_data->eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;
    frame_data->eFrameScanMode = E_MI_SYS_FRAME_SCAN_MODE_PROGRESSIVE;
    frame_data->eFieldType = E_MI_SYS_FIELDTYPE_NONE;
    frame_data->eTileMode = E_MI_SYS_FRAME_TILE_MODE_NONE;
    
    return 0;
}

static int dma_blit_texture_to_texture(MMIYOO_TextureData *src_texture, SDL_Rect *src_rect,
                                       MMIYOO_TextureData *dst_texture, SDL_Rect *dst_rect) {
    MI_SYS_FrameData_t src_frame, dst_frame;
    MI_SYS_WindowRect_t mi_src_rect, mi_dst_rect;
    
    if (texture_to_framedata(src_texture, &src_frame) != 0) return -1;
    if (texture_to_framedata(dst_texture, &dst_frame) != 0) return -1;
    
    // Set up source rectangle
    mi_src_rect.u16X = src_rect ? src_rect->x : 0;
    mi_src_rect.u16Y = src_rect ? src_rect->y : 0;
    mi_src_rect.u16Width = src_rect ? src_rect->w : src_texture->width;
    mi_src_rect.u16Height = src_rect ? src_rect->h : src_texture->height;
    
    // Set up destination rectangle  
    mi_dst_rect.u16X = dst_rect ? dst_rect->x : 0;
    mi_dst_rect.u16Y = dst_rect ? dst_rect->y : 0;
    mi_dst_rect.u16Width = dst_rect ? dst_rect->w : dst_texture->width;
    mi_dst_rect.u16Height = dst_rect ? dst_rect->h : dst_texture->height;
    
    // Perform hardware-accelerated blit using DMA
    return MI_SYS_BufBlitPa(&dst_frame, &mi_dst_rect, &src_frame, &mi_src_rect);
}

// Forward declaration for batching functions
static SDL_Rect
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

static SDL_bool
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



static void MMIYOO_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
}

// Convert SDL pixel format to MI_GFX format with comprehensive validation
static MI_GFX_ColorFmt_e sdl_to_mi_gfx_format(Uint32 sdl_format, int *bits_per_pixel, const char **format_name) {
    switch(sdl_format) {
        case SDL_PIXELFORMAT_RGB565:
            *bits_per_pixel = 16;
            *format_name = "RGB565";
            return E_MI_GFX_FMT_RGB565;
            
        case SDL_PIXELFORMAT_BGR565:
            *bits_per_pixel = 16;
            *format_name = "BGR565";
            return E_MI_GFX_FMT_BGR565;
            
        case SDL_PIXELFORMAT_ARGB8888:
            *bits_per_pixel = 32;
            *format_name = "ARGB8888";
            return E_MI_GFX_FMT_ARGB8888;
            
        case SDL_PIXELFORMAT_RGBA8888:
            *bits_per_pixel = 32;
            *format_name = "RGBA8888->ARGB8888";
            return E_MI_GFX_FMT_ARGB8888;  /* Force ARGB8888 for consistency with framebuffer */
            
        case SDL_PIXELFORMAT_ABGR8888:
            *bits_per_pixel = 32;
            *format_name = "ABGR8888";
            return E_MI_GFX_FMT_ABGR8888;
            
        case SDL_PIXELFORMAT_BGRA8888:
            *bits_per_pixel = 32;
            *format_name = "BGRA8888";
            return E_MI_GFX_FMT_BGRA8888;
            
        case SDL_PIXELFORMAT_ARGB1555:
            *bits_per_pixel = 16;
            *format_name = "ARGB1555";
            return E_MI_GFX_FMT_ARGB1555;
            
        case SDL_PIXELFORMAT_ARGB4444:
            *bits_per_pixel = 16;
            *format_name = "ARGB4444";
            return E_MI_GFX_FMT_ARGB4444;
            
        case SDL_PIXELFORMAT_RGBA4444:
            *bits_per_pixel = 16;
            *format_name = "RGBA4444";
            return E_MI_GFX_FMT_RGBA4444;
            
        default:
            *bits_per_pixel = 32;
            *format_name = "ARGB8888 (fallback)";
            return E_MI_GFX_FMT_ARGB8888;
    }
}

/* Best-fit scan: smallest cached block with alloc_size >= requested_size,
 * rejecting anything that would waste more than MMIYOO_TEXTURE_POOL_SLACK_MUL
 * times the request. Swap-remove on hit (order doesn't matter -- bounded,
 * not LRU-sensitive). */
static SDL_bool
mmiyoo_pool_try_acquire(unsigned int requested_size, MI_PHY *out_phy, void **out_vir,
                         unsigned int *out_alloc_size)
{
    int i;
    int best = -1;
    SDL_bool found;

    SDL_AtomicLock(&mmiyoo_texture_pool_lock);

    for (i = 0; i < mmiyoo_texture_pool_count; ++i) {
        unsigned int candidate = mmiyoo_texture_pool[i].alloc_size;
        if (candidate >= requested_size && candidate <= requested_size * MMIYOO_TEXTURE_POOL_SLACK_MUL) {
            if (best < 0 || candidate < mmiyoo_texture_pool[best].alloc_size) {
                best = i;
            }
        }
    }

    found = (best >= 0);
    if (found) {
        *out_phy = mmiyoo_texture_pool[best].phyAddr;
        *out_vir = mmiyoo_texture_pool[best].virAddr;
        *out_alloc_size = mmiyoo_texture_pool[best].alloc_size;

        mmiyoo_texture_pool_bytes -= mmiyoo_texture_pool[best].alloc_size;
        mmiyoo_texture_pool[best] = mmiyoo_texture_pool[mmiyoo_texture_pool_count - 1];
        --mmiyoo_texture_pool_count;
    }

    SDL_AtomicUnlock(&mmiyoo_texture_pool_lock);
    return found;
}

/* Called from MMIYOO_DestroyTexture instead of an immediate Munmap+MMA_Free.
 * Caches the block if there's room under both caps; otherwise frees it for
 * real immediately (identical to the pre-pooling behavior). The caller must
 * already have flushed any GFX fences touching this block (MMIYOO_DestroyTexture
 * already does this unconditionally before calling this). */
static void
mmiyoo_pool_release_or_free(MI_PHY phyAddr, void *virAddr, unsigned int alloc_size)
{
    SDL_bool cached = SDL_FALSE;

    SDL_AtomicLock(&mmiyoo_texture_pool_lock);

    if (mmiyoo_texture_pool_enabled &&
        mmiyoo_texture_pool_count < MMIYOO_TEXTURE_POOL_MAX_ENTRIES &&
        mmiyoo_texture_pool_bytes + alloc_size <= mmiyoo_texture_pool_max_bytes) {
        mmiyoo_texture_pool[mmiyoo_texture_pool_count].phyAddr = phyAddr;
        mmiyoo_texture_pool[mmiyoo_texture_pool_count].virAddr = virAddr;
        mmiyoo_texture_pool[mmiyoo_texture_pool_count].alloc_size = alloc_size;
        ++mmiyoo_texture_pool_count;
        mmiyoo_texture_pool_bytes += alloc_size;
        cached = SDL_TRUE;
    } else {
        ++mmiyoo_pool_evictions;
    }

    SDL_AtomicUnlock(&mmiyoo_texture_pool_lock);

    if (!cached) {
        if (virAddr) {
            MI_SYS_Munmap(virAddr, alloc_size);
        }
        if (phyAddr) {
            MI_SYS_MMA_Free(phyAddr);
        }
    }
}

/* Really frees every cached block. Used at renderer teardown and as a
 * one-shot release valve right before MMIYOO_CreateTexture would otherwise
 * report OOM. */
static void
mmiyoo_pool_drain(void)
{
    int i;
    int count;
    MMIYOO_PooledBlock local[MMIYOO_TEXTURE_POOL_MAX_ENTRIES];

    SDL_AtomicLock(&mmiyoo_texture_pool_lock);
    count = mmiyoo_texture_pool_count;
    for (i = 0; i < count; ++i) {
        local[i] = mmiyoo_texture_pool[i];
    }
    mmiyoo_texture_pool_count = 0;
    mmiyoo_texture_pool_bytes = 0;
    SDL_AtomicUnlock(&mmiyoo_texture_pool_lock);

    for (i = 0; i < count; ++i) {
        if (local[i].virAddr) {
            MI_SYS_Munmap(local[i].virAddr, local[i].alloc_size);
        }
        if (local[i].phyAddr) {
            MI_SYS_MMA_Free(local[i].phyAddr);
        }
    }
}

static int MMIYOO_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    MMIYOO_TextureData *mmiyoo_texture;
    MI_GFX_ColorFmt_e mi_format;
    const char *format_name;

    MMIYOO_VERBOSE_LOG("CreateTexture: format=0x%08x size=%dx%d access=%d",
                        texture->format, texture->w, texture->h, texture->access);

    mmiyoo_texture = (MMIYOO_TextureData *)SDL_calloc(1, sizeof(*mmiyoo_texture));

    if(!mmiyoo_texture) {
        printf("ERROR: MMIYOO_CreateTexture calloc failed!\n");
        fflush(stdout);
        return SDL_OutOfMemory();
    }

    mmiyoo_texture->width = texture->w;
    mmiyoo_texture->height = texture->h;
    mmiyoo_texture->format = texture->format;

    {
        int bits_temp;
        mi_format = sdl_to_mi_gfx_format(texture->format, &bits_temp, &format_name);
        mmiyoo_texture->bits = (unsigned int)bits_temp;
    }

    mmiyoo_texture->mi_format = mi_format;
    mmiyoo_texture->bytes_per_pixel = mmiyoo_texture->bits / 8;

    if (mmiyoo_texture->bytes_per_pixel == 0) {
        mmiyoo_texture->bytes_per_pixel = 4;
    }

    mmiyoo_texture->pitch = mmiyoo_texture->width * mmiyoo_texture->bytes_per_pixel;
    
    mmiyoo_texture->pitch = (mmiyoo_texture->pitch + 63) & ~63;

    mmiyoo_texture->size = mmiyoo_texture->height * mmiyoo_texture->pitch;

    mmiyoo_texture->size = MMIYOO_ALIGN_SYS(mmiyoo_texture->size);
        
    mmiyoo_texture->uses_msys_memory = SDL_TRUE;

    if (mmiyoo_texture_pool_enabled &&
        mmiyoo_pool_try_acquire(mmiyoo_texture->size, &mmiyoo_texture->phyAddr,
                                 &mmiyoo_texture->virAddr, &mmiyoo_texture->alloc_size)) {
        SDL_assert(mmiyoo_texture->alloc_size >= mmiyoo_texture->size);
        ++mmiyoo_pool_hits;
        MMIYOO_VERBOSE_LOG("CreateTexture: pool HIT size=%u alloc_size=%u (hits=%u misses=%u)",
                            mmiyoo_texture->size, mmiyoo_texture->alloc_size,
                            mmiyoo_pool_hits, mmiyoo_pool_misses);
    } else {
        ++mmiyoo_pool_misses;

        if (MI_SYS_MMA_Alloc(NULL, mmiyoo_texture->size, &mmiyoo_texture->phyAddr) != MI_SUCCESS) {
            if (mmiyoo_texture_pool_enabled && mmiyoo_texture_pool_count > 0) {
                ++mmiyoo_pool_drains;
                mmiyoo_pool_drain();
                MMIYOO_VERBOSE_LOG("CreateTexture: alloc failed, drained pool, retrying size=%u",
                                    mmiyoo_texture->size);
            }
            if (MI_SYS_MMA_Alloc(NULL, mmiyoo_texture->size, &mmiyoo_texture->phyAddr) != MI_SUCCESS) {
                /* A caller that keeps retrying the same failing allocation
                 * every frame (observed: one texture retried ~100+ times
                 * in under two minutes) would otherwise spam this on every
                 * attempt -- each print+fflush is synchronous I/O over a
                 * 115200-baud serial console (console=ttyS0,115200), a real
                 * per-frame cost. Rate-limit instead of silencing outright. */
                static unsigned int oom_log_count = 0;
                ++oom_log_count;
                if (oom_log_count <= 3 || (oom_log_count % 50) == 0) {
                    printf("ERROR: MI_SYS_MMA_Alloc FAILED size=%u (%ux%u %s) live=%d (occurrence #%u)\n",
                           mmiyoo_texture->size, mmiyoo_texture->width, mmiyoo_texture->height,
                           format_name, mmiyoo_texture_live_count, oom_log_count);
                    fflush(stdout);
                }
                SDL_free(mmiyoo_texture);
                return SDL_OutOfMemory();
            }
        }

        mmiyoo_texture->alloc_size = mmiyoo_texture->size;

        if (MI_SYS_Mmap(mmiyoo_texture->phyAddr, mmiyoo_texture->size, &mmiyoo_texture->virAddr, TRUE) != MI_SUCCESS) {
            printf("ERROR: MI_SYS_Mmap FAILED phyAddr=0x%llx size=%u (%ux%u %s) live=%d\n",
                   (unsigned long long)mmiyoo_texture->phyAddr, mmiyoo_texture->size,
                   mmiyoo_texture->width, mmiyoo_texture->height, format_name, mmiyoo_texture_live_count);
            fflush(stdout);
            MI_SYS_MMA_Free(mmiyoo_texture->phyAddr);
            SDL_free(mmiyoo_texture);
            return SDL_OutOfMemory();
        }
    }

    mmiyoo_texture->data = mmiyoo_texture->virAddr;

    mmiyoo_texture->gfx_surface.u32Width = mmiyoo_texture->width;
    mmiyoo_texture->gfx_surface.u32Height = mmiyoo_texture->height;
    mmiyoo_texture->gfx_surface.u32Stride = mmiyoo_texture->pitch;
    mmiyoo_texture->gfx_surface.phyAddr = mmiyoo_texture->phyAddr;
    mmiyoo_texture->gfx_surface.eColorFmt = mi_format;

    texture->driverdata = mmiyoo_texture;

    ++mmiyoo_texture_live_count;

    return 0;
}

static int MMIYOO_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    MMIYOO_TextureData *mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;

    *pixels = mmiyoo_texture->data;
    *pitch = mmiyoo_texture->pitch;
    return 0;
}


static int MMIYOO_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    MMIYOO_TextureData *mmiyoo_texture;
    Uint32 dst_pitch;
    Uint32 bytes_per_pixel;

    mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;
    
    if (!mmiyoo_texture || !pixels) {
        MMIYOO_LOG_WARN("UpdateTexture: invalid args texture=%p pixels=%p", (void*)texture, pixels);
        return -1;
    }

#ifdef MMIYOO
    /* Ensure any pending hardware operations touching this texture are completed before we overwrite it. */
    GFX_FlushTextureFences();
#endif

    dst_pitch = mmiyoo_texture->pitch;
    bytes_per_pixel = mmiyoo_texture->bytes_per_pixel ? mmiyoo_texture->bytes_per_pixel : SDL_BYTESPERPIXEL(texture->format);

    if (!bytes_per_pixel) {
        MMIYOO_LOG_WARN("UpdateTexture: bytes_per_pixel resolved to 0 (format=0x%x)", texture->format);
        return -1;
    }

    if (rect) {
        Uint8 *dst_row;
        const Uint8 *src_row;
        size_t row_bytes;
        dst_row = (Uint8*)mmiyoo_texture->virAddr + rect->y * dst_pitch + rect->x * bytes_per_pixel;
        src_row = (const Uint8*)pixels;
        row_bytes = (size_t)rect->w * bytes_per_pixel;

        for (int row = 0; row < rect->h; ++row) {
            neon_memcpy(dst_row, src_row, row_bytes);
            dst_row += dst_pitch;
            src_row += pitch;
        }

        if (mmiyoo_texture->uses_msys_memory) {
            size_t flush_size = (size_t)rect->h * dst_pitch;
            MMIYOO_FlushInvCacheRange((Uint8*)mmiyoo_texture->virAddr + rect->y * dst_pitch, flush_size);
        }
    } else {
        Uint8 *dst_row;
        const Uint8 *src_row;
        size_t row_bytes;

        dst_row = (Uint8*)mmiyoo_texture->virAddr;
        src_row = (const Uint8*)pixels;
        row_bytes = (size_t)texture->w * bytes_per_pixel;
        for (int row = 0; row < texture->h; ++row) {
            neon_memcpy(dst_row, src_row, row_bytes);
            dst_row += dst_pitch;
            src_row += pitch;
        }

        if (mmiyoo_texture->uses_msys_memory) {
            size_t modified_size = (size_t)texture->h * dst_pitch;
            MMIYOO_FlushInvCacheRange(mmiyoo_texture->virAddr, modified_size);
        }
    }

    return 0;
}

static void MMIYOO_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_Rect rect = {0};
    MMIYOO_TextureData *mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;

    rect.x = 0;
    rect.y = 0;
    rect.w = texture->w;
    rect.h = texture->h;
    MMIYOO_UpdateTexture(renderer, texture, &rect, mmiyoo_texture->data, mmiyoo_texture->pitch);
}

static void MMIYOO_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
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

static int MMIYOO_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    const SDL_Rect *viewport;
    
    if (!renderer || !cmd || !data) {
        return -1;
    }
    
    viewport = &cmd->data.viewport.rect;
    
    if (viewport->x < -1000000 || viewport->x > 1000000 ||
        viewport->y < -1000000 || viewport->y > 1000000) {
        return 0;
    }
    
    if (viewport->w == 0 || viewport->h == 0) {
        data->viewport_enabled = SDL_FALSE;
    } else {
        data->viewport = *viewport;
        data->viewport_enabled = SDL_TRUE;
    }

    return 0;
}

static int MMIYOO_QueueSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    
    if (!data || !cmd) {
        return -1;
    }
    
    data->draw_color_r = cmd->data.color.r;
    data->draw_color_g = cmd->data.color.g;
    data->draw_color_b = cmd->data.color.b;
    data->draw_color_a = cmd->data.color.a;

    return 0;
}

// Forward declaration
static int MMIYOO_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count);

static int MMIYOO_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    SDL_FRect *rects;
    int i;

    if (count <= 0) {
        return 0;
    }

    rects = (SDL_FRect *)SDL_stack_alloc(SDL_FRect, count);
    if (!rects) {
        SDL_OutOfMemory();
        return -1;
    }

    for (i = 0; i < count; i++) {
        rects[i].x = points[i].x;
        rects[i].y = points[i].y;
        rects[i].w = 1.0f;
        rects[i].h = 1.0f;
    }

    if (MMIYOO_QueueFillRects(renderer, cmd, rects, count) < 0) {
        SDL_stack_free(rects);
        return -1;
    }

    SDL_stack_free(rects);
    return 0;
}

static int MMIYOO_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    SDL_FPoint *verts;

    if (count < 2) {
        return 0;
    }

    verts = (SDL_FPoint *)SDL_AllocateRenderVertices(renderer, count * sizeof(SDL_FPoint), 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    neon_memcpy(verts, points, count * sizeof(SDL_FPoint));
    cmd->data.draw.count = count;

    return 0;
}

static int MMIYOO_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                                const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
                                int num_vertices, const void *indices, int num_indices, int size_indices,
                                float scale_x, float scale_y)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MMIYOO_GeometryFillVertex *verts;
    MMIYOO_GeometryTextureData *texdata;
    int i;
    int count;

    count = indices ? num_indices : num_vertices;
    if (count < 3) {
        return 0;
    }

    if (texture != NULL) {
        int total_r = 0;
        int total_g = 0;
        int total_b = 0;
        int total_a = 0;
        int tri_count = count / 3;
        MMIYOO_GeometryTextureTri *tris;

        if (tri_count < 1) {
            return 0;
        }

        texdata = (MMIYOO_GeometryTextureData *)SDL_AllocateRenderVertices(renderer,
                       sizeof(*texdata) + (size_t)tri_count * sizeof(MMIYOO_GeometryTextureTri),
                       0, &cmd->data.draw.first);
        if (!texdata) {
            return -1;
        }
        tris = (MMIYOO_GeometryTextureTri *)(texdata + 1);

        for (i = 0; i < tri_count; ++i) {
            int k;
            float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
            float min_u = 0.0f, min_v = 0.0f, max_u = 1.0f, max_v = 1.0f;
            int src_x, src_y, src_w, src_h;
            MMIYOO_GeometryTextureTri *tri = &tris[i];

            for (k = 0; k < 3; ++k) {
                int vi = i * 3 + k;
                int j;
                const float *xy_ptr;
                float x, y;

                if (indices) {
                    if (size_indices == 4) {
                        j = ((const Uint32 *)indices)[vi];
                    } else if (size_indices == 2) {
                        j = ((const Uint16 *)indices)[vi];
                    } else {
                        j = ((const Uint8 *)indices)[vi];
                    }
                } else {
                    j = vi;
                }

                xy_ptr = (const float *)((const char *)xy + j * xy_stride);
                x = xy_ptr[0] * scale_x;
                y = xy_ptr[1] * scale_y;

                if (k == 0) {
                    min_x = max_x = x;
                    min_y = max_y = y;
                } else {
                    min_x = SDL_min(min_x, x);
                    min_y = SDL_min(min_y, y);
                    max_x = SDL_max(max_x, x);
                    max_y = SDL_max(max_y, y);
                }

                if (uv) {
                    const float *uv_ptr = (const float *)((const char *)uv + j * uv_stride);
                    float u = uv_ptr[0];
                    float v = uv_ptr[1];
                    if (k == 0) {
                        min_u = max_u = u;
                        min_v = max_v = v;
                    } else {
                        min_u = SDL_min(min_u, u);
                        min_v = SDL_min(min_v, v);
                        max_u = SDL_max(max_u, u);
                        max_v = SDL_max(max_v, v);
                    }
                }

                if (color) {
                    const SDL_Color *vertex_color = (const SDL_Color *)((const char *)color + j * color_stride);
                    total_r += vertex_color->r;
                    total_g += vertex_color->g;
                    total_b += vertex_color->b;
                    total_a += vertex_color->a;
                } else {
                    total_r += data->draw_color_r;
                    total_g += data->draw_color_g;
                    total_b += data->draw_color_b;
                    total_a += data->draw_color_a;
                }
            }

            min_u = SDL_clamp(min_u, 0.0f, 1.0f);
            min_v = SDL_clamp(min_v, 0.0f, 1.0f);
            max_u = SDL_clamp(max_u, 0.0f, 1.0f);
            max_v = SDL_clamp(max_v, 0.0f, 1.0f);

            src_x = SDL_max(0, (int)SDL_floorf(min_u * (float)texture->w));
            src_y = SDL_max(0, (int)SDL_floorf(min_v * (float)texture->h));
            src_w = SDL_max(1, (int)SDL_ceilf(max_u * (float)texture->w) - src_x);
            src_h = SDL_max(1, (int)SDL_ceilf(max_v * (float)texture->h) - src_y);

            if (src_x + src_w > texture->w) {
                src_w = texture->w - src_x;
            }
            if (src_y + src_h > texture->h) {
                src_h = texture->h - src_y;
            }

            tri->srcrect.x = src_x;
            tri->srcrect.y = src_y;
            tri->srcrect.w = src_w;
            tri->srcrect.h = src_h;

            tri->dstrect.x = min_x;
            tri->dstrect.y = min_y;
            tri->dstrect.w = SDL_max(1.0f, max_x - min_x);
            tri->dstrect.h = SDL_max(1.0f, max_y - min_y);
            tri->skip = SDL_FALSE;

            /* SDL_MMIYOO_GEOMETRY_QUICKPATH: a glyph quad's two triangles
             * (an axis-aligned diagonal split) always compute identical
             * rects here -- drop the second's blit rather than draw the
             * same rect twice. Local per-pair check only, never merges
             * across distinct glyphs. */
            if (data->geometry_quickpath_enabled && (i & 1) == 1) {
                MMIYOO_GeometryTextureTri *prev = &tris[i - 1];
                if (tri->srcrect.x == prev->srcrect.x && tri->srcrect.y == prev->srcrect.y &&
                    tri->srcrect.w == prev->srcrect.w && tri->srcrect.h == prev->srcrect.h &&
                    tri->dstrect.x == prev->dstrect.x && tri->dstrect.y == prev->dstrect.y &&
                    tri->dstrect.w == prev->dstrect.w && tri->dstrect.h == prev->dstrect.h) {
                    tri->skip = SDL_TRUE;
                }
            }
        }

        texdata->target_surface = data->current_target_surface;
        texdata->is_target_texture = data->is_target_texture;
        texdata->tri_count = tri_count;

        cmd->data.draw.count = 1;
        cmd->data.draw.r = (Uint8)(total_r / count);
        cmd->data.draw.g = (Uint8)(total_g / count);
        cmd->data.draw.b = (Uint8)(total_b / count);
        cmd->data.draw.a = (Uint8)(total_a / count);
        return 0;
    }

    verts = (MMIYOO_GeometryFillVertex *)SDL_AllocateRenderVertices(renderer, count * sizeof(MMIYOO_GeometryFillVertex), 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (i = 0; i < count; ++i) {
        int j;
        const float *xy_ptr;
        SDL_Color vertex_color;

        if (indices) {
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else {
                j = ((const Uint8 *)indices)[i];
            }
        } else {
            j = i;
        }

        xy_ptr = (const float *)((const char *)xy + j * xy_stride);
        verts[i].x = xy_ptr[0] * scale_x;
        verts[i].y = xy_ptr[1] * scale_y;

        if (color) {
            vertex_color = *(const SDL_Color *)((const char *)color + j * color_stride);
        } else {
            vertex_color.r = data->draw_color_r;
            vertex_color.g = data->draw_color_g;
            vertex_color.b = data->draw_color_b;
            vertex_color.a = data->draw_color_a;
        }
        verts[i].color = vertex_color;
    }

    return 0;
}

static int MMIYOO_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count)
{
    SDL_Rect *verts;
    int i;

    if (count <= 0 || !rects) {
        return 0;
    }

    verts = (SDL_Rect *)SDL_AllocateRenderVertices(renderer, count * sizeof(SDL_Rect), 0, &cmd->data.draw.first);
    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++) {
        SDL_Rect *dst = &verts[i];
        dst->x = (int)SDL_floorf(rects[i].x);
        dst->y = (int)SDL_floorf(rects[i].y);
        dst->w = (int)SDL_floorf(rects[i].w);
        dst->h = (int)SDL_floorf(rects[i].h);

            if (dst->w < 1) {
                dst->w = (dst->w < 0) ? 0 : 1;
            }
            if (dst->h < 1) {
                dst->h = (dst->h < 0) ? 0 : 1;
            }

    }

    return 0;
}

static int MMIYOO_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_FRect *dstrect)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    Uint8 *queue_data;
    SDL_Rect *verts;
    MI_GFX_Surface_t *queued_surface;
    SDL_bool *queued_is_target_texture;

    if (!srcrect || !dstrect) {
        return -1;
    }

    queue_data = (Uint8 *)SDL_AllocateRenderVertices(renderer,
                                                       2 * sizeof(SDL_Rect) + sizeof(MI_GFX_Surface_t) + sizeof(SDL_bool),
                                                       0, &cmd->data.draw.first);
    if (!queue_data) {
        return -1;
    }

    cmd->data.draw.count = 1;

    verts = (SDL_Rect *)queue_data;
    neon_memcpy(&verts[0], srcrect, sizeof(SDL_Rect));
    verts[1].x = (int)SDL_floorf(dstrect->x);
    verts[1].y = (int)SDL_floorf(dstrect->y);
    verts[1].w = (int)SDL_floorf(dstrect->w);
    verts[1].h = (int)SDL_floorf(dstrect->h);

    if (verts[1].w < 0) {
        verts[1].w = 0;
    }
    if (verts[1].h < 0) {
        verts[1].h = 0;
    }

    queued_surface = (MI_GFX_Surface_t *)(queue_data + 2 * sizeof(SDL_Rect));
    neon_memcpy(queued_surface, &data->current_target_surface, sizeof(MI_GFX_Surface_t));

    queued_is_target_texture = (SDL_bool *)(queue_data + 2 * sizeof(SDL_Rect) + sizeof(MI_GFX_Surface_t));
    *queued_is_target_texture = data->is_target_texture;
    return 0;
}

static MI_GFX_Rotate_e
MMIYOO_AddRotations(MI_GFX_Rotate_e base, MI_GFX_Rotate_e extra)
{
    int total = ((int)base + (int)extra) & 3;
    return (MI_GFX_Rotate_e)total;
}

static SDL_bool
MMIYOO_RotationSwapsAxes(MI_GFX_Rotate_e rotation)
{
    return (rotation == E_MI_GFX_ROTATE_90) || (rotation == E_MI_GFX_ROTATE_270);
}

static MI_GFX_Mirror_e
MMIYOO_FlipToMirror(SDL_RendererFlip flip)
{
    SDL_RendererFlip filtered = (SDL_RendererFlip)(flip & (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

    if (filtered == SDL_FLIP_HORIZONTAL) {
        return E_MI_GFX_MIRROR_HORIZONTAL;
    } else if (filtered == SDL_FLIP_VERTICAL) {
        return E_MI_GFX_MIRROR_VERTICAL;
    } else if (filtered == (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL)) {
        return E_MI_GFX_MIRROR_BOTH;
    }

    return E_MI_GFX_MIRROR_NONE;
}

/* Core-content integer-scale upscaler: MI_GFX_BitBlit has no interpolation control, so we scale in software instead.
 * Architecture credit: Onion's RetroArch miyoomini driver (github.com/OnionUI/RetroArch, GPLv3) -- no code copied.
 * Scaler functions: neon-arm-library-miyoo (github.com/XK9274/neon-arm-library-miyoo), already linked but unused until now.
 * Research notes: notes/blurry-scaling-investigation.md (gitignored, local only). */

typedef void (*MMIYOO_NeonScaleFunc)(void *src, void *dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp);

static int
MMIYOO_ClampHorizontalMul(int raw)
{
    /* neon-arm-library-miyoo only provides horizontal multipliers of 1, 2, or 4. */
    if (raw >= 4) return 4;
    if (raw >= 2) return 2;
    return 1;
}

static int
MMIYOO_ClampVerticalMul(int raw)
{
    if (raw < 1) return 1;
    if (raw > 4) return 4;
    return raw;
}

static MMIYOO_NeonScaleFunc
MMIYOO_PickScaleFunc(int xmul, int ymul, unsigned int bytes_per_pixel, SDL_bool neon_safe)
{
    if (bytes_per_pixel == 2) {
        switch (xmul) {
            case 1:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x1_n16 : scale1x1_c16);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x2_n16 : scale1x2_c16);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x3_n16 : scale1x3_c16);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x4_n16 : scale1x4_c16);
                }
            case 2:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x1_n16 : scale2x1_c16);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x2_n16 : scale2x2_c16);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x3_n16 : scale2x3_c16);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x4_n16 : scale2x4_c16);
                }
            default:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x1_n16 : scale4x1_c16);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x2_n16 : scale4x2_c16);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x3_n16 : scale4x3_c16);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x4_n16 : scale4x4_c16);
                }
        }
    } else {
        switch (xmul) {
            case 1:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x1_n32 : scale1x1_c32);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x2_n32 : scale1x2_c32);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x3_n32 : scale1x3_c32);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale1x4_n32 : scale1x4_c32);
                }
            case 2:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x1_n32 : scale2x1_c32);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x2_n32 : scale2x2_c32);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x3_n32 : scale2x3_c32);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale2x4_n32 : scale2x4_c32);
                }
            default:
                switch (ymul) {
                    case 1: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x1_n32 : scale4x1_c32);
                    case 2: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x2_n32 : scale4x2_c32);
                    case 3: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x3_n32 : scale4x3_c32);
                    default: return (MMIYOO_NeonScaleFunc)(neon_safe ? scale4x4_n32 : scale4x4_c32);
                }
        }
    }
}

/* Grows the persistent scale scratch buffer to hold required_size bytes; never shrinks; freed in MMIYOO_DestroyRenderer. */
static SDL_bool
MMIYOO_EnsureScaleScratch(MMIYOO_RenderData *data, unsigned int required_size)
{
    MI_PHY new_phy = 0;
    void *new_vir = NULL;

    required_size = MMIYOO_ALIGN_SYS(required_size);

    if (data->scale_scratch_vir && data->scale_scratch_alloc_size >= required_size) {
        return SDL_TRUE;
    }

    /* Latched after a failed grow so we don't retry the same failing MI_SYS_MMA_Alloc every frame. */
    if (data->scale_scratch_alloc_failed) {
        return SDL_FALSE;
    }

    if (MI_SYS_MMA_Alloc(NULL, required_size, &new_phy) != MI_SUCCESS) {
        data->scale_scratch_alloc_failed = SDL_TRUE;
        return SDL_FALSE;
    }
    if (MI_SYS_Mmap(new_phy, required_size, &new_vir, TRUE) != MI_SUCCESS) {
        MI_SYS_MMA_Free(new_phy);
        data->scale_scratch_alloc_failed = SDL_TRUE;
        return SDL_FALSE;
    }

    if (data->scale_scratch_vir) {
        MI_SYS_Munmap(data->scale_scratch_vir, data->scale_scratch_alloc_size);
        MI_SYS_MMA_Free(data->scale_scratch_phy);
    }

    data->scale_scratch_phy = new_phy;
    data->scale_scratch_vir = new_vir;
    data->scale_scratch_alloc_size = required_size;
    return SDL_TRUE;
}

/* Generic downscale dispatch: source and destination dims are runtime
 * arguments (see downscale_area_n32 in neon-arm-library-miyoo), so unlike
 * MMIYOO_PickScaleFunc there's no per-ratio switch -- pixel format is the
 * only gate. NULL means "can't handle this format", not "unknown ratio". */
typedef void (*MMIYOO_NeonDownscaleFunc)(void *src, void *dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp, uint32_t dw, uint32_t dh);

static MMIYOO_NeonDownscaleFunc
MMIYOO_PickDownscaleFunc(unsigned int bytes_per_pixel)
{
    if (bytes_per_pixel == 4) {
        return (MMIYOO_NeonDownscaleFunc)downscale_area_n32;
    }
    return NULL;
}

/* Downscales an oversized render-target texture into a framebuffer-sized
 * scratch buffer. Returns SDL_FALSE when the input is unsupported. */
static SDL_bool
MMIYOO_TryDownscaleCompositeCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                                  MMIYOO_TextureData *src_texture_data,
                                  SDL_Rect *src, SDL_Rect *dst,
                                  SDL_BlendMode blend_mode,
                                  const void **pixels, int *pitch, MI_PHY *src_phy)
{
    int framebuffer_width;
    int framebuffer_height;
    unsigned int dst_stride;
    unsigned int required_size;
    unsigned int bpp;
    MMIYOO_NeonDownscaleFunc downscale_func;

    (void)texture;

    if (blend_mode != SDL_BLENDMODE_NONE) {
        return SDL_FALSE;
    }
    if (src->w <= 0 || src->h <= 0) {
        return SDL_FALSE;
    }

    bpp = src_texture_data->bytes_per_pixel;
    downscale_func = MMIYOO_PickDownscaleFunc(bpp);

    framebuffer_width = MMIYOO_GetFramebufferWidth(data);
    framebuffer_height = MMIYOO_GetFramebufferHeight(data);

    if (!downscale_func || framebuffer_width <= 0 || framebuffer_height <= 0) {
        if (!data->downscale_unsupported_warned) {
            MMIYOO_LOG_WARN("MMIYOO_TryDownscaleCompositeCopy: unsupported oversized composite (bpp=%u src=%dx%d fb=%dx%d), dropping draw",
                            bpp, src->w, src->h, framebuffer_width, framebuffer_height);
            data->downscale_unsupported_warned = SDL_TRUE;
        }
        return SDL_FALSE;
    }

    dst_stride = (unsigned int)(framebuffer_width * (int)bpp);
    dst_stride = (dst_stride + 15) & ~15u;
    required_size = dst_stride * (unsigned int)framebuffer_height;
    required_size = MMIYOO_ALIGN_SYS(required_size);

    if (!MMIYOO_EnsureScaleScratch(data, required_size)) {
        return SDL_FALSE;
    }

    /* The source here is a render-target texture the game has been drawing
     * into all frame via GFX BitBlit/QuickFill/DrawLine, each of which only
     * queues a fence (GFX_AddTextureFence) without waiting on it. Without
     * this flush the NEON downscale below reads src_origin on the CPU while
     * the GPU may still be mid-write to that same memory -- a torn-frame
     * race that gets worse the faster frames are produced. */
    GFX_FlushTextureFences();

    {
        const Uint8 *src_origin = (const Uint8 *)*pixels +
                                   (size_t)src->y * (size_t)*pitch +
                                   (size_t)src->x * (size_t)bpp;

        /* Avoid the slow C fallback in this per-frame compositor. */
        if ( ((uintptr_t)src_origin & 3) || ((uintptr_t)data->scale_scratch_vir & 3) ||
             (((uint32_t)*pitch) & 3) || (dst_stride & 3) ||
             ((uint32_t)framebuffer_width > DOWNSCALE_AREA_MAX_DW) ) {
            if (!data->downscale_unsupported_warned) {
                MMIYOO_LOG_WARN("MMIYOO_TryDownscaleCompositeCopy: misaligned/oversized input would hit the slow C downscale fallback (src=%p pitch=%d dst_stride=%u fb_w=%d), dropping draw instead",
                                (void *)src_origin, *pitch, dst_stride, framebuffer_width);
                data->downscale_unsupported_warned = SDL_TRUE;
            }
            return SDL_FALSE;
        }

        /* Keep the CPU/GFX handoff cache-coherent across both MI_SYS buffers. */
        MMIYOO_FlushInvCacheRange((void *)src_origin,
                                  (size_t)src->h * (size_t)*pitch);

        downscale_func((void *)src_origin, data->scale_scratch_vir,
                       (uint32_t)src->w, (uint32_t)src->h,
                       (uint32_t)*pitch, dst_stride,
                       (uint32_t)framebuffer_width, (uint32_t)framebuffer_height);

        MMIYOO_FlushInvCacheRange(data->scale_scratch_vir, required_size);
    }

    dst->x = 0;
    dst->y = 0;
    dst->w = framebuffer_width;
    dst->h = framebuffer_height;
    src->x = 0;
    src->y = 0;
    src->w = framebuffer_width;
    src->h = framebuffer_height;

    *pixels = data->scale_scratch_vir;
    *pitch = (int)dst_stride;
    *src_phy = data->scale_scratch_phy;

    return SDL_TRUE;
}

/* Software integer-scales into a letterboxed scratch buffer for an unscaled GFX_Copy present; returns SDL_FALSE if scaling doesn't apply. */
static SDL_bool
MMIYOO_TryIntegerScaleCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                            MMIYOO_TextureData *src_texture_data,
                            SDL_Rect *src, SDL_Rect *dst,
                            SDL_BlendMode blend_mode,
                            const void **pixels, int *pitch, MI_PHY *src_phy)
{
    int xmul_raw, ymul_raw, xmul, ymul;
    int scaled_w, scaled_h;
    unsigned int dst_stride;
    unsigned int required_size;
    MMIYOO_NeonScaleFunc scale_func;
    const Uint8 *src_origin;
    SDL_bool neon_safe;
    unsigned int bpp;

    if (!data->integer_scale_enabled) {
        return SDL_FALSE;
    }
    if (blend_mode != SDL_BLENDMODE_NONE) {
        return SDL_FALSE;
    }
    if (texture->access != SDL_TEXTUREACCESS_STREAMING) {
        return SDL_FALSE;
    }
    if (src->w <= 0 || src->h <= 0 || dst->w <= 0 || dst->h <= 0) {
        return SDL_FALSE;
    }
    if (src->w == dst->w && src->h == dst->h) {
        return SDL_FALSE;
    }

    bpp = src_texture_data->bytes_per_pixel;
    if (bpp != 2 && bpp != 4) {
        return SDL_FALSE;
    }

    xmul_raw = dst->w / src->w;
    ymul_raw = dst->h / src->h;
    if (xmul_raw < 1 || ymul_raw < 1) {
        /* These scalers only upscale; leave downscales to the hardware path. */
        return SDL_FALSE;
    }

    xmul = MMIYOO_ClampHorizontalMul(xmul_raw);
    ymul = MMIYOO_ClampVerticalMul(ymul_raw);

    scaled_w = src->w * xmul;
    scaled_h = src->h * ymul;
    /* MI_GFX_BitBlit faults on a source stride that isn't 16-byte-aligned. */
    dst_stride = (unsigned int)(scaled_w * (int)bpp);
    dst_stride = (dst_stride + 15) & ~15u;
    required_size = dst_stride * (unsigned int)scaled_h;
    required_size = MMIYOO_ALIGN_SYS(required_size);

    if (!MMIYOO_EnsureScaleScratch(data, required_size)) {
        return SDL_FALSE;
    }

    /* TODO: add an alignment-aware 16bpp NEON path once odd-pixel crops can
     * be handled without falling back to the C scaler. */
    /* The fixed-ratio NEON kernels require 4-byte aligned source/destination
     * addresses and strides. In particular, a clipped 16bpp source with an
     * odd X offset is only 2-byte aligned. Use the corresponding C kernel
     * for that case rather than issuing an unsafe NEON load/store sequence. */
    src_origin = (const Uint8 *)*pixels +
                 (size_t)src->y * (size_t)*pitch +
                 (size_t)src->x * (size_t)bpp;
    neon_safe = (((uintptr_t)src_origin & 3) == 0 &&
                 ((uintptr_t)data->scale_scratch_vir & 3) == 0 &&
                 (((uint32_t)*pitch & 3) == 0) &&
                 ((dst_stride & 3) == 0));
    scale_func = MMIYOO_PickScaleFunc(xmul, ymul, bpp, neon_safe);
    scale_func((void *)src_origin, data->scale_scratch_vir,
               (uint32_t)src->w, (uint32_t)src->h,
               (uint32_t)*pitch, dst_stride);

    dst->x += (dst->w - scaled_w) / 2;
    dst->y += (dst->h - scaled_h) / 2;
    dst->w = scaled_w;
    dst->h = scaled_h;

    src->x = 0;
    src->y = 0;
    src->w = scaled_w;
    src->h = scaled_h;

    *pixels = data->scale_scratch_vir;
    *pitch = (int)dst_stride;
    *src_phy = data->scale_scratch_phy;

    return SDL_TRUE;
}

/* Stretches a full-texture blit to fill the framebuffer when dst is smaller than the screen. */
static void
MMIYOO_TryStretchFillCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                           SDL_Rect *src, SDL_Rect *dst, SDL_BlendMode blend_mode)
{
    int framebuffer_width;
    int framebuffer_height;

    if (blend_mode != SDL_BLENDMODE_NONE) {
        return;
    }
    if (texture->access != SDL_TEXTUREACCESS_STREAMING) {
        return;
    }
    if (src->w <= 0 || src->h <= 0 || dst->w <= 0 || dst->h <= 0) {
        return;
    }
    if (src->x != 0 || src->y != 0 || src->w != texture->w || src->h != texture->h) {
        /* Deliberate crop/sub-rect draw, not a full-content present. */
        return;
    }

    framebuffer_width = MMIYOO_GetFramebufferWidth(data);
    framebuffer_height = MMIYOO_GetFramebufferHeight(data);

    if (dst->x == 0 && dst->y == 0 && dst->w == framebuffer_width && dst->h == framebuffer_height) {
        /* Already fills the framebuffer. */
        return;
    }

    dst->x = 0;
    dst->y = 0;
    dst->w = framebuffer_width;
    dst->h = framebuffer_height;
}

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
                 Uint8 mod_a)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MMIYOO_TextureData *src_texture_data;
    MMIYOO_TextureData *dst_texture_data;
    SDL_bool used_integer_scale = SDL_FALSE;
    SDL_bool used_downscale = SDL_FALSE;
    int pitch = 0;
    MI_PHY src_phy = 0;
    int copy_result;
    SDL_Rect dst;
    SDL_Rect src;
    SDL_Rect clip_rect;
    SDL_Rect hw_dst;
    SDL_Rect hw_clip;
    SDL_Rect prepared_dst;
    SDL_bool clip_enabled = SDL_FALSE;
    SDL_bool allow_src_adjust;
    MI_GFX_Rotate_e base_rotation;
    MI_GFX_Rotate_e effective_rotation;
    MI_GFX_Mirror_e mirror;
    SDL_FPoint center = rotation_center;
    const float dst_width = dstrect->w;
    const float dst_height = dstrect->h;
    const float dst_x = dstrect->x;
    const float dst_y = dstrect->y;
    float center_abs_x;
    float center_abs_y;
    const SDL_bool swaps_axes = MMIYOO_RotationSwapsAxes(extra_rotation);
    const float rotated_width_f = swaps_axes ? dst_height : dst_width;
    const float rotated_height_f = swaps_axes ? dst_width : dst_height;
    int rotated_width;
    int rotated_height;

    if (data->collect_frame_timing) {
        data->timing_blit_calls += 1;
    }

    if (dst_width <= 0.0f || dst_height <= 0.0f) {
        return 0;
    }

    rotated_width = SDL_max(1, (int)SDL_lroundf(rotated_width_f));
    rotated_height = SDL_max(1, (int)SDL_lroundf(rotated_height_f));

    center_abs_x = dst_x + center.x;
    center_abs_y = dst_y + center.y;

    dst.w = rotated_width;
    dst.h = rotated_height;
    dst.x = (int)SDL_lroundf(center_abs_x - ((float)rotated_width * 0.5f));
    dst.y = (int)SDL_lroundf(center_abs_y - ((float)rotated_height * 0.5f));

    src = *srcrect;

    prepared_dst = dst;
    allow_src_adjust = (extra_rotation == E_MI_GFX_ROTATE_0) && (flip == SDL_FLIP_NONE);

    if (!MMIYOO_PrepareDrawRect(renderer, data, &prepared_dst,
                                allow_src_adjust ? &src : NULL,
                                &clip_rect, &clip_enabled)) {
        return 0;
    }

    dst = prepared_dst;

    if (!texture) {
        MMIYOO_LOG_WARN("QueueCopy: texture is NULL");
        return 0;
    }

    src_texture_data = (MMIYOO_TextureData *)texture->driverdata;
    if (!src_texture_data) {
        MMIYOO_LOG_WARN("QueueCopy: texture driverdata missing for %p", (void *)texture);
        return 0;
    }

    if (!pixels) {
        pixels = src_texture_data->virAddr;
    }

    pitch = src_texture_data->pitch;
    if ((pitch <= 0) || (pixels == NULL)) {
        MMIYOO_LOG_WARN("QueueCopy: invalid pitch=%d or pixels=%p", pitch, pixels);
        return 0;
    }

    base_rotation = data->is_target_texture ? E_MI_GFX_ROTATE_0 : E_MI_GFX_ROTATE_180;
    effective_rotation = MMIYOO_AddRotations(base_rotation, extra_rotation);
    mirror = MMIYOO_FlipToMirror(flip);

    if (!data->is_target_texture) {
        int framebuffer_width = MMIYOO_GetFramebufferWidth(data);
        int framebuffer_height = MMIYOO_GetFramebufferHeight(data);

        if (texture->w > framebuffer_width || texture->h > framebuffer_height) {
            /* Oversized render-target texture composited to the screen: the
             * ROTATE_180 path below hangs MI_GFX on a source larger than the
             * panel (see WHERE3.md). Downscale to panel size first so
             * GFX_Copy never sees the oversized+rotated combination. */
            used_downscale = MMIYOO_TryDownscaleCompositeCopy(data, texture, src_texture_data, &src, &dst,
                                                                blend_mode, &pixels, &pitch, &src_phy);
            if (!used_downscale) {
                /* Degenerate/unsupported input already logged once inside
                 * MMIYOO_TryDownscaleCompositeCopy -- never fall through to
                 * GFX_Copy with the untouched oversized source. */
                return 0;
            }
        } else if (extra_rotation == E_MI_GFX_ROTATE_0 && flip == SDL_FLIP_NONE) {
            /* Core-content software integer-scale; only applies to the default/window target, see MMIYOO_TryIntegerScaleCopy. */
            used_integer_scale = MMIYOO_TryIntegerScaleCopy(data, texture, src_texture_data, &src, &dst,
                                                              blend_mode, &pixels, &pitch, &src_phy);
            if (!used_integer_scale) {
                MMIYOO_TryStretchFillCopy(data, texture, &src, &dst, blend_mode);
            }
        }
    }

    // DMA optimization: if both source and target are MI_SYS textures, use hardware blit
    if (data->is_target_texture && data->boundTarget && blend_mode == SDL_BLENDMODE_NONE) {
        dst_texture_data = (MMIYOO_TextureData *)data->boundTarget->driverdata;
        
        if (dst_texture_data && dst_texture_data->phyAddr && src_texture_data->phyAddr) {
            if (dma_blit_texture_to_texture(src_texture_data, &src, dst_texture_data, &dst) == MI_SUCCESS) {
                return 0;
            }
        }
    }

    hw_dst = dst;
    hw_clip = clip_rect;

    if (!data->is_target_texture) {
        /* Framebuffer draws need to counter the upside-down panel */
        const int framebuffer_width = MMIYOO_GetFramebufferWidth(data);
        const int framebuffer_height = MMIYOO_GetFramebufferHeight(data);
        hw_dst.x = framebuffer_width - dst.x - dst.w;
        hw_dst.y = framebuffer_height - dst.y - dst.h;

        if (clip_enabled) {
            hw_clip.x = framebuffer_width - clip_rect.x - clip_rect.w;
            hw_clip.y = framebuffer_height - clip_rect.y - clip_rect.h;
        }
    }

    /* Enhanced logging for overlay texture operations */
    if (texture && !data->is_target_texture) {
        Uint32 format;
        int access, w, h;
        if (SDL_QueryTexture(texture, &format, &access, &w, &h) == 0) {
            if (access == SDL_TEXTUREACCESS_TARGET) {
                data->texture_blitted_to_screen = SDL_TRUE;
            }
        }
    }

    if (src_texture_data && !used_integer_scale && !used_downscale) {
        src_phy = src_texture_data->phyAddr;
    }

    if (!data->is_target_texture && SDL_GetHintBoolean("SDL_MMIYOO_DEBUG_LOG", SDL_FALSE)) {
        MMIYOO_LOG_WARN("SCALEDBG QueueCopy: fb=%dx%d target_surf=%ux%u int_scale=%d src=(%d,%d,%d,%d) dst=(%d,%d,%d,%d) hw_dst=(%d,%d,%d,%d)",
                        MMIYOO_GetFramebufferWidth(data), MMIYOO_GetFramebufferHeight(data),
                        (unsigned int)data->current_target_surface.u32Width, (unsigned int)data->current_target_surface.u32Height,
                        (int)used_integer_scale,
                        src.x, src.y, src.w, src.h,
                        dst.x, dst.y, dst.w, dst.h,
                        hw_dst.x, hw_dst.y, hw_dst.w, hw_dst.h);
    }

    copy_result = GFX_Copy(pixels, src_phy, src, hw_dst, pitch, (int)effective_rotation, mirror, blend_mode, &data->current_target_surface,
                           clip_enabled ? &hw_clip : NULL, clip_enabled,
                           texture->format, src_texture_data->mi_format, src_texture_data->bytes_per_pixel,
                           mod_r, mod_g, mod_b, mod_a);
    if (copy_result != 0) {
        MMIYOO_LOG_WARN("QueueCopy: GFX_Copy failed (result=%d)", copy_result);
    }
    return 0;
}

typedef struct
{
    SDL_Rect srcrect;
    float dst_x;
    float dst_y;
    float dst_w;
    float dst_h;
    double angle;
    SDL_FPoint center;
    SDL_bool has_explicit_center;
    SDL_RendererFlip flip;
    MI_GFX_Surface_t target_surface;
    SDL_bool is_target_texture;
} MMIYOO_CopyExData;

static int MMIYOO_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                              const SDL_Rect *srcrect, const SDL_FRect *dstrect,
                              const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MMIYOO_CopyExData *copydata;

    (void)texture;

    if (!srcrect || !dstrect) {
        return -1;
    }

    copydata = (MMIYOO_CopyExData *)SDL_AllocateRenderVertices(renderer, sizeof(*copydata), 0, &cmd->data.draw.first);
    if (!copydata) {
        return -1;
    }

    cmd->data.draw.count = 1;

    copydata->srcrect = *srcrect;
    copydata->dst_x = dstrect->x;
    copydata->dst_y = dstrect->y;
    copydata->dst_w = dstrect->w;
    copydata->dst_h = dstrect->h;
    copydata->angle = angle;
    copydata->flip = flip;

    if (center) {
        copydata->center = *center;
        copydata->has_explicit_center = SDL_TRUE;
    } else {
        copydata->center.x = dstrect->w * 0.5f;
        copydata->center.y = dstrect->h * 0.5f;
        copydata->has_explicit_center = SDL_FALSE;
    }

    copydata->target_surface = data->current_target_surface;
    copydata->is_target_texture = data->is_target_texture;

    return 0;
}

static void
MMIYOO_ProcessFillCommand(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices)
{
    const int count = (int)cmd->data.draw.count;
    SDL_Rect *rects;
    Uint32 color;
    int i;
    MMIYOO_LineBatch skinny_batch;
    MMIYOO_RectBatch rect_batch;

    if (count <= 0) {
        return;
    }

    rects = (SDL_Rect *)(((Uint8 *)vertices) + cmd->data.draw.first);
    if (!rects) {
        MMIYOO_LOG_WARN("ProcessFillCommand: vertex data missing");
        return;
    }
    color = MMIYOO_PackColor(cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a);

    MMIYOO_LineBatchReset(&skinny_batch);
    MMIYOO_RectBatchReset(&rect_batch);

    for (i = 0; i < count; i++) {
        SDL_Rect dst = rects[i];
        SDL_Rect clip_rect;
        SDL_bool clip_enabled = SDL_FALSE;

        if (dst.w <= 0 || dst.h <= 0) {
            continue;
        }

        if (!MMIYOO_PrepareDrawRect(renderer, data, &dst, NULL, &clip_rect, &clip_enabled)) {
            continue;
        }

        if (dst.w == 1 && dst.h == 1) {
            if (skinny_batch.active) {
                MMIYOO_LineBatchFlush(renderer, data, &skinny_batch, color);
            }
            if (rect_batch.active) {
                MMIYOO_RectBatchFlush(renderer, data, &rect_batch, color);
            }
            MMIYOO_ExecuteQuickFill(data, &dst, color);
            continue;
        }

        if (dst.w == 1 || dst.h == 1) {
            SDL_bool vertical = (dst.w == 1);
            if (rect_batch.active) {
                MMIYOO_RectBatchFlush(renderer, data, &rect_batch, color);
            }
            MMIYOO_LineBatchAccumulate(renderer, data, &skinny_batch, &dst, vertical, color);
        } else {
            if (skinny_batch.active) {
                MMIYOO_LineBatchFlush(renderer, data, &skinny_batch, color);
            }
            MMIYOO_RectBatchAccumulate(renderer, data, &rect_batch, &dst, color);
        }
    }

    if (skinny_batch.active) {
        MMIYOO_LineBatchFlush(renderer, data, &skinny_batch, color);
    }
    if (rect_batch.active) {
        MMIYOO_RectBatchFlush(renderer, data, &rect_batch, color);
    }
}

static void
MMIYOO_ProcessDrawLines(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices)
{
    int count = (int)cmd->data.draw.count;
    SDL_FPoint *points;
    Uint32 color;
    int i;

    (void)renderer;

    if (count < 2 || !vertices) {
        return;
    }

    points = (SDL_FPoint *)(((Uint8 *)vertices) + cmd->data.draw.first);
    color = MMIYOO_PackColor(data->draw_color_r, data->draw_color_g, data->draw_color_b, data->draw_color_a);

    for (i = 0; i < count - 1; ++i) {
        float x0 = points[i].x;
        float y0 = points[i].y;
        float x1 = points[i + 1].x;
        float y1 = points[i + 1].y;
        float minx;
        float maxx;
        float miny;
        float maxy;
        SDL_Rect line_rect;
        SDL_Rect prepared_rect;
        float clip_x0;
        float clip_y0;
        float clip_x1;
        float clip_y1;

        MMIYOO_ApplyViewportToPoint(data, &x0, &y0);
        MMIYOO_ApplyViewportToPoint(data, &x1, &y1);

        minx = SDL_min(x0, x1);
        maxx = SDL_max(x0, x1);
        miny = SDL_min(y0, y1);
        maxy = SDL_max(y0, y1);

        {
            int min_ix = (int)SDL_floorf(minx);
            int max_ix = (int)SDL_ceilf(maxx);
            int min_iy = (int)SDL_floorf(miny);
            int max_iy = (int)SDL_ceilf(maxy);

            line_rect.x = min_ix;
            line_rect.y = min_iy;
            line_rect.w = SDL_max(1, max_ix - min_ix + 1);
            line_rect.h = SDL_max(1, max_iy - min_iy + 1);
        }

        prepared_rect = line_rect;
        if (!MMIYOO_PrepareDrawRect(renderer, data, &prepared_rect, NULL, NULL, NULL)) {
            continue;
        }

        clip_x0 = x0;
        clip_y0 = y0;
        clip_x1 = x1;
        clip_y1 = y1;

        if (!MMIYOO_ClipLineToRect(&clip_x0, &clip_y0, &clip_x1, &clip_y1, &prepared_rect)) {
            continue;
        }

        if (SDL_fabsf(clip_x0 - clip_x1) < 0.0005f || SDL_fabsf(clip_y0 - clip_y1) < 0.0005f) {
            SDL_Rect span_rect;
            SDL_Rect clamped_rect;

            if (SDL_fabsf(clip_x0 - clip_x1) < 0.0005f) {
                int ix = MMIYOO_FloatToPixel(clip_x0);
                int iy0 = MMIYOO_FloatToPixel(SDL_min(clip_y0, clip_y1));
                int iy1 = MMIYOO_FloatToPixel(SDL_max(clip_y0, clip_y1));
                if (iy1 < iy0) {
                    int tmp = iy0;
                    iy0 = iy1;
                    iy1 = tmp;
                }
                span_rect.x = ix;
                span_rect.y = iy0;
                span_rect.w = 1;
                span_rect.h = SDL_max(1, iy1 - iy0 + 1);
            } else {
                int iy = MMIYOO_FloatToPixel(clip_y0);
                int ix0 = MMIYOO_FloatToPixel(SDL_min(clip_x0, clip_x1));
                int ix1 = MMIYOO_FloatToPixel(SDL_max(clip_x0, clip_x1));
                if (ix1 < ix0) {
                    int tmp = ix0;
                    ix0 = ix1;
                    ix1 = tmp;
                }
                span_rect.x = ix0;
                span_rect.y = iy;
                span_rect.w = SDL_max(1, ix1 - ix0 + 1);
                span_rect.h = 1;
            }

            clamped_rect = span_rect;
            if (!SDL_IntersectRect(&span_rect, &prepared_rect, &clamped_rect)) {
                continue;
            }

            if (!SDL_RectEmpty(&clamped_rect)) {
                MMIYOO_ExecuteQuickFill(data, &clamped_rect, color);
            }
            continue;
        }

        if (!MMIYOO_ExecuteDrawLine(data, clip_x0, clip_y0, clip_x1, clip_y1, color)) {
            MMIYOO_LOG_WARN("DrawLines: hardware line draw failed");
        }
    }
}

static void
MMIYOO_ProcessGeometry(SDL_Renderer *renderer, MMIYOO_RenderData *data, const SDL_RenderCommand *cmd, void *vertices)
{
    int count = (int)cmd->data.draw.count;
    MMIYOO_GeometryFillVertex *verts;
    int i;

    if (cmd->data.draw.texture) {
        MMIYOO_GeometryTextureData *texdata;
        MMIYOO_GeometryTextureTri *tris;
        MI_GFX_Surface_t saved_surface;
        SDL_bool saved_is_target_texture;
        int t;

        if (count <= 0 || !vertices) {
            return;
        }

        texdata = (MMIYOO_GeometryTextureData *)(((Uint8 *)vertices) + cmd->data.draw.first);
        if (!texdata) {
            MMIYOO_LOG_WARN("ProcessGeometry: textured geometry data missing");
            return;
        }
        tris = (MMIYOO_GeometryTextureTri *)(texdata + 1);

        saved_surface = data->current_target_surface;
        saved_is_target_texture = data->is_target_texture;

        data->current_target_surface = texdata->target_surface;
        data->is_target_texture = texdata->is_target_texture;

        /* One blit per triangle -- each quad (2 triangles) samples its own
         * atlas subrect, e.g. one glyph out of a packed font atlas. */
        for (t = 0; t < texdata->tri_count; ++t) {
            MMIYOO_GeometryTextureTri *tri = &tris[t];
            SDL_FPoint center;
            SDL_bool used_quickfill = SDL_FALSE;

            if (tri->skip) {
                continue;
            }

            /* Fast path: a 1x1 source texture (SDL2's "tint a single pixel"
             * trick for solid/gradient-panel fills -- common in Ozone menu
             * geometry) has no spatial detail to lose, so a fully-opaque
             * draw can be redirected to the same proven QuickFill span path
             * the untextured fill geometry already uses, instead of a real
             * hardware texture blit per triangle. MI_GFX_QuickFill has no
             * blend flags at all, so this only ever engages when the result
             * is fully opaque -- never for real translucency, which still
             * goes through My_QueueCopy below unchanged. Caveat: uses the
             * triangle's bounding box (not exact triangle coverage), which
             * is exact for the common case (2 right triangles forming an
             * axis-aligned rect) but could over-paint a non-rectangular
             * triangle (e.g. a diagonal divider) slightly beyond its true
             * edge -- watch for that specifically when checking this. */
            if (cmd->data.draw.texture->w == 1 && cmd->data.draw.texture->h == 1) {
                MMIYOO_TextureData *tex1x1 = (MMIYOO_TextureData *)cmd->data.draw.texture->driverdata;
                if (tex1x1 && tex1x1->virAddr && tex1x1->bytes_per_pixel == 4 &&
                    (tex1x1->mi_format == E_MI_GFX_FMT_ABGR8888 || tex1x1->mi_format == E_MI_GFX_FMT_ARGB8888)) {
                    const Uint8 *px = (const Uint8 *)tex1x1->virAddr;
                    Uint8 pr, pg, pb, pa;

                    if (tex1x1->mi_format == E_MI_GFX_FMT_ABGR8888) {
                        pr = px[0]; pg = px[1]; pb = px[2]; pa = px[3];
                    } else {
                        pb = px[0]; pg = px[1]; pr = px[2]; pa = px[3];
                    }

                    if (pa == 255 && cmd->data.draw.a == 255) {
                        Uint8 fr = (Uint8)(((int)pr * cmd->data.draw.r + 127) / 255);
                        Uint8 fg = (Uint8)(((int)pg * cmd->data.draw.g + 127) / 255);
                        Uint8 fb = (Uint8)(((int)pb * cmd->data.draw.b + 127) / 255);
                        SDL_Rect prepared_dst;

                        prepared_dst.x = (int)SDL_floorf(tri->dstrect.x);
                        prepared_dst.y = (int)SDL_floorf(tri->dstrect.y);
                        prepared_dst.w = SDL_max(1, (int)SDL_ceilf(tri->dstrect.w));
                        prepared_dst.h = SDL_max(1, (int)SDL_ceilf(tri->dstrect.h));

                        if (MMIYOO_PrepareDrawRect(renderer, data, &prepared_dst, NULL, NULL, NULL)) {
                            MMIYOO_ExecuteQuickFill(data, &prepared_dst, MMIYOO_PackColor(fr, fg, fb, 255));
                            used_quickfill = SDL_TRUE;
                        }
                    }
                }
            }

            if (used_quickfill) {
                continue;
            }

            center.x = tri->dstrect.w * 0.5f;
            center.y = tri->dstrect.h * 0.5f;

            if (My_QueueCopy(renderer, cmd->data.draw.texture, NULL,
                             &tri->srcrect, &tri->dstrect,
                             cmd->data.draw.blend,
                             E_MI_GFX_ROTATE_0, SDL_FLIP_NONE, center,
                             cmd->data.draw.r, cmd->data.draw.g,
                             cmd->data.draw.b, cmd->data.draw.a) != 0) {
                MMIYOO_LOG_WARN("ProcessGeometry: textured triangle copy failed");
            }
        }

        data->current_target_surface = saved_surface;
        data->is_target_texture = saved_is_target_texture;
        return;
    }

    if (count < 3 || !vertices) {
        return;
    }

    verts = (MMIYOO_GeometryFillVertex *)(((Uint8 *)vertices) + cmd->data.draw.first);

    for (i = 0; i <= count - 3; i += 3) {
        SDL_FPoint p0 = { verts[i].x, verts[i].y };
        SDL_FPoint p1 = { verts[i + 1].x, verts[i + 1].y };
        SDL_FPoint p2 = { verts[i + 2].x, verts[i + 2].y };
        SDL_Color c0 = verts[i].color;
        SDL_Color c1 = verts[i + 1].color;
        SDL_Color c2 = verts[i + 2].color;
        SDL_Color avg_color;
        Uint32 packed_color;
        SDL_Rect bounds;
        SDL_Rect clipped_bounds;

        avg_color.r = (Uint8)(((int)c0.r + (int)c1.r + (int)c2.r) / 3);
        avg_color.g = (Uint8)(((int)c0.g + (int)c1.g + (int)c2.g) / 3);
        avg_color.b = (Uint8)(((int)c0.b + (int)c1.b + (int)c2.b) / 3);
        avg_color.a = (Uint8)(((int)c0.a + (int)c1.a + (int)c2.a) / 3);

        packed_color = MMIYOO_PackColor(avg_color.r, avg_color.g, avg_color.b, avg_color.a);

        MMIYOO_ApplyViewportToPoint(data, &p0.x, &p0.y);
        MMIYOO_ApplyViewportToPoint(data, &p1.x, &p1.y);
        MMIYOO_ApplyViewportToPoint(data, &p2.x, &p2.y);

        {
            float min_x = SDL_min(SDL_min(p0.x, p1.x), p2.x);
            float min_y = SDL_min(SDL_min(p0.y, p1.y), p2.y);
            float max_x = SDL_max(SDL_max(p0.x, p1.x), p2.x);
            float max_y = SDL_max(SDL_max(p0.y, p1.y), p2.y);

            bounds.x = (int)SDL_floorf(min_x);
            bounds.y = (int)SDL_floorf(min_y);
            bounds.w = SDL_max(1, (int)SDL_floorf(max_x) - bounds.x + 1);
            bounds.h = SDL_max(1, (int)SDL_floorf(max_y) - bounds.y + 1);
        }

        clipped_bounds = bounds;
        if (!MMIYOO_PrepareDrawRect(renderer, data, &clipped_bounds, NULL, NULL, NULL)) {
            continue;
        }

        MMIYOO_DrawFilledTriangle(data, &p0, &p1, &p2, &clipped_bounds, packed_color);
    }
}

static int MMIYOO_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    Uint64 timing_start = data->collect_frame_timing ? SDL_GetPerformanceCounter() : 0;

    while (cmd) {
        Uint64 cmd_start = data->collect_frame_timing ? SDL_GetPerformanceCounter() : 0;

        switch (cmd->command) {
            case SDL_RENDERCMD_SETVIEWPORT:
                MMIYOO_QueueSetViewport(renderer, cmd);
                break;

            case SDL_RENDERCMD_SETDRAWCOLOR:
                MMIYOO_QueueSetDrawColor(renderer, cmd);
                break;

            case SDL_RENDERCMD_SETCLIPRECT:
                MMIYOO_UpdateClipState(data, cmd->data.cliprect.enabled, &cmd->data.cliprect.rect);
                break;

            case SDL_RENDERCMD_CLEAR:
            {
                SDL_Rect bounds = MMIYOO_GetTargetBounds(data);
                Uint32 color = MMIYOO_PackColor(cmd->data.color.r, cmd->data.color.g, cmd->data.color.b, cmd->data.color.a);
                MMIYOO_ExecuteQuickFill(data, &bounds, color);
                break;
            }

            case SDL_RENDERCMD_DRAW_POINTS:
            case SDL_RENDERCMD_FILL_RECTS:
                MMIYOO_ProcessFillCommand(renderer, data, cmd, vertices);
                break;

            case SDL_RENDERCMD_COPY:
            {
                Uint8 *queue_data;
                SDL_Rect *verts;
                SDL_Rect src;
                SDL_Rect dst;
                SDL_FRect dstf;
                SDL_Texture *texture;
                const void *pixels = NULL;
                MI_GFX_Surface_t *queued_surface;
                SDL_bool *queued_is_target_texture;
                MI_GFX_Surface_t saved_surface;
                SDL_bool saved_is_target_texture;
                SDL_FPoint default_center;

                queue_data = ((Uint8 *)vertices) + cmd->data.draw.first;
                verts = (SDL_Rect *)queue_data;
                src = verts[0];
                dst = verts[1];
                texture = cmd->data.draw.texture;

                queued_surface = (MI_GFX_Surface_t *)(queue_data + 2 * sizeof(SDL_Rect));
                queued_is_target_texture = (SDL_bool *)(queue_data + 2 * sizeof(SDL_Rect) + sizeof(MI_GFX_Surface_t));

                if (!texture) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY: missing texture reference");
                    break;
                }

                dstf.x = (float)dst.x;
                dstf.y = (float)dst.y;
                dstf.w = (float)dst.w;
                dstf.h = (float)dst.h;
                default_center.x = dstf.w * 0.5f;
                default_center.y = dstf.h * 0.5f;

                saved_surface = data->current_target_surface;
                saved_is_target_texture = data->is_target_texture;

                data->current_target_surface = *queued_surface;
                data->is_target_texture = *queued_is_target_texture;

                if (My_QueueCopy(renderer, texture, pixels, &src, &dstf, cmd->data.draw.blend,
                                  E_MI_GFX_ROTATE_0, SDL_FLIP_NONE, default_center,
                                  cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a) != 0) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY: My_QueueCopy failed");
                }

                data->current_target_surface = saved_surface;
                data->is_target_texture = saved_is_target_texture;
                break;
            }

            case SDL_RENDERCMD_NO_OP:
                break;

            case SDL_RENDERCMD_DRAW_LINES:
                MMIYOO_ProcessDrawLines(renderer, data, cmd, vertices);
                break;

            case SDL_RENDERCMD_COPY_EX:
            {
                MMIYOO_CopyExData *copydata;
                SDL_Texture *texture = cmd->data.draw.texture;
                SDL_FRect dstf;
                MI_GFX_Surface_t saved_surface;
                SDL_bool saved_is_target_texture;
                MI_GFX_Rotate_e extra_rotation = E_MI_GFX_ROTATE_0;
                double normalized_angle;
                double snapped_angle;
                int multiple;

                if (!texture) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY_EX: missing texture reference");
                    break;
                }

                copydata = (MMIYOO_CopyExData *)(((Uint8 *)vertices) + cmd->data.draw.first);
                if (!copydata) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY_EX: copy data missing");
                    break;
                }

                dstf.x = copydata->dst_x;
                dstf.y = copydata->dst_y;
                dstf.w = copydata->dst_w;
                dstf.h = copydata->dst_h;

                normalized_angle = copydata->angle;
                while (normalized_angle >= 360.0) {
                    normalized_angle -= 360.0;
                }
                while (normalized_angle < 0.0) {
                    normalized_angle += 360.0;
                }

                multiple = (int)SDL_lround(normalized_angle / 90.0);
                snapped_angle = (double)multiple * 90.0;
                if (SDL_fabs(normalized_angle - snapped_angle) > 0.01) {
                    if (!g_warned_copyex_angle) {
                        MMIYOO_LOG_WARN("RunCommandQueue COPY_EX: angle %.3f unsupported, drawing with closest multiple (%.0f)",
                                         copydata->angle, snapped_angle);
                        g_warned_copyex_angle = SDL_TRUE;
                    }
                }

                multiple &= 3;
                switch (multiple) {
                    case 0:
                        extra_rotation = E_MI_GFX_ROTATE_0;
                        break;
                    case 1:
                        extra_rotation = E_MI_GFX_ROTATE_90;
                        break;
                    case 2:
                        extra_rotation = E_MI_GFX_ROTATE_180;
                        break;
                    case 3:
                    default:
                        extra_rotation = E_MI_GFX_ROTATE_270;
                        break;
                }

                saved_surface = data->current_target_surface;
                saved_is_target_texture = data->is_target_texture;

                data->current_target_surface = copydata->target_surface;
                data->is_target_texture = copydata->is_target_texture;

                if (My_QueueCopy(renderer, texture, NULL, &copydata->srcrect, &dstf, cmd->data.draw.blend,
                                  extra_rotation, copydata->flip, copydata->center,
                                  cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a) != 0) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY_EX: My_QueueCopy failed");
                }

                data->current_target_surface = saved_surface;
                data->is_target_texture = saved_is_target_texture;
                break;
            }

            case SDL_RENDERCMD_GEOMETRY:
                MMIYOO_ProcessGeometry(renderer, data, cmd, vertices);
                break;

            default:
                break;
        }

        if (data->collect_frame_timing) {
            Uint64 elapsed = SDL_GetPerformanceCounter() - cmd_start;
            switch (cmd->command) {
                case SDL_RENDERCMD_CLEAR:
                case SDL_RENDERCMD_DRAW_POINTS:
                case SDL_RENDERCMD_FILL_RECTS:
                    data->timing_fill_ticks += elapsed;
                    break;
                case SDL_RENDERCMD_COPY:
                case SDL_RENDERCMD_COPY_EX:
                    data->timing_copy_ticks += elapsed;
                    break;
                case SDL_RENDERCMD_GEOMETRY:
                    data->timing_geometry_ticks += elapsed;
                    break;
                case SDL_RENDERCMD_DRAW_LINES:
                    data->timing_lines_ticks += elapsed;
                    break;
                default:
                    data->timing_misc_ticks += elapsed;
                    break;
            }
        }

        cmd = cmd->next;
    }
    if (data->collect_frame_timing) {
        data->timing_command_queue_ticks += SDL_GetPerformanceCounter() - timing_start;
    }
    return 1;
}

static int MMIYOO_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 pixel_format, void *pixels, int pitch)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MMIYOO_TextureData *src_texture;
    void *src_pixels;

    /* Only the target-texture case has a CPU-mapped, format-known source to
     * read from (mirrors SW_RenderReadPixels in src/render/software/SDL_render_sw.c).
     * The default/window target's only CPU-visible buffer is gfx.back
     * (src/video/mmiyoo/SDL_video_mmiyoo.h), whose runtime pixel format was
     * never verified on-device -- guessing it wrong would silently hand back
     * corrupted pixels instead of a clean error, so that case stays
     * unsupported, same as PSP_RenderReadPixels for the same reason. */
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
mmiyoo_present_vsync_active(SDL_bool renderer_vsync_requested)
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

static void
mmiyoo_update_present_vsync_flag(SDL_Renderer *renderer, SDL_bool renderer_vsync_requested)
{
    if (mmiyoo_present_vsync_active(renderer_vsync_requested)) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    } else {
        renderer->info.flags &= ~SDL_RENDERER_PRESENTVSYNC;
    }
}

static void MMIYOO_RenderPresent(SDL_Renderer *renderer)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;
    MI_GFX_Surface_t target_surface_before = data->current_target_surface;
    SDL_bool was_target_texture = data->is_target_texture;

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

        if (!data->is_target_texture || data->texture_blitted_to_screen) {
            GFX_SwapBuffers(data->vsync);
            mmiyoo_update_present_vsync_flag(renderer, data->vsync);
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
                double frame_count = (double)data->timing_frames;
                double fps = frame_count / window_s;
                double cmdqueue_ms_per_frame = (double)data->timing_command_queue_ticks * 1000.0 / (double)freq / frame_count;
                double present_ms_per_frame = (double)data->timing_present_ticks * 1000.0 / (double)freq / frame_count;
                double blits_per_frame = (double)data->timing_blit_calls / frame_count;
                double fill_ms = (double)data->timing_fill_ticks * 1000.0 / (double)freq / frame_count;
                double copy_ms = (double)data->timing_copy_ticks * 1000.0 / (double)freq / frame_count;
                double geometry_ms = (double)data->timing_geometry_ticks * 1000.0 / (double)freq / frame_count;
                double lines_ms = (double)data->timing_lines_ticks * 1000.0 / (double)freq / frame_count;
                double misc_ms = (double)data->timing_misc_ticks * 1000.0 / (double)freq / frame_count;

                SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                            "MMIYOO frame timing: fps=%.1f cmdQueue=%.2fms/frame present=%.2fms/frame blits=%.1f/frame",
                            fps, cmdqueue_ms_per_frame, present_ms_per_frame, blits_per_frame);
                SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
                            "MMIYOO cmdQueue breakdown: fill=%.2fms copy=%.2fms geometry=%.2fms lines=%.2fms misc=%.2fms (ms/frame)",
                            fill_ms, copy_ms, geometry_ms, lines_ms, misc_ms);

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

static void MMIYOO_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    MMIYOO_TextureData *mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;

    if (mmiyoo_texture) {
        if (mmiyoo_texture->uses_msys_memory) {
            /* A BitBlit/QuickFill/DrawLine reading this texture's phyAddr may
               still be in flight on the GFX engine; wait it out before the
               memory is freed and potentially handed to an unrelated
               allocation. */
            GFX_FlushTextureFences();

            if (mmiyoo_texture->phyAddr) {
                mmiyoo_pool_release_or_free(mmiyoo_texture->phyAddr, mmiyoo_texture->virAddr,
                                             mmiyoo_texture->alloc_size);
            }
        } else if (mmiyoo_texture->virAddr) {
            SDL_free(mmiyoo_texture->virAddr);
        }

        SDL_free(mmiyoo_texture);
        texture->driverdata = NULL;
        --mmiyoo_texture_live_count;
        MMIYOO_VERBOSE_LOG("DestroyTexture: texture=%p total_live=%d", (void *)texture, mmiyoo_texture_live_count);
    }
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
    mmiyoo_pool_drain();

    SDL_free(renderer);
}

static int MMIYOO_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    MMIYOO_RenderData *data = (MMIYOO_RenderData *)renderer->driverdata;

    data->vsync = (vsync != 0) ? SDL_TRUE : SDL_FALSE;
    mmiyoo_update_present_vsync_flag(renderer, data->vsync);
    return 0;
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
    renderer->DestroyTexture = MMIYOO_DestroyTexture;
    renderer->DestroyRenderer = MMIYOO_DestroyRenderer;
    renderer->SetVSync = MMIYOO_SetVSync;
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
        const char *pool_hint = SDL_GetHint("SDL_MMIYOO_TEXTURE_POOL");
        if (pool_hint) {
            mmiyoo_texture_pool_enabled = (SDL_atoi(pool_hint) != 0) ? SDL_TRUE : SDL_FALSE;
        }
    }

    {
        const char *pool_bytes_hint = SDL_GetHint("SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES");
        if (pool_bytes_hint) {
            int v = SDL_atoi(pool_bytes_hint);
            if (v > 0) {
                mmiyoo_texture_pool_max_bytes = (unsigned int)v;
            }
        }
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
    mmiyoo_update_present_vsync_flag(renderer, data->vsync);

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
        /* Allows an oversized (up to 800x600) render target; MMIYOO_TryDownscaleCompositeCopy handles the composite. */
        .max_texture_width = 800,
        .max_texture_height = 600,
    }
};

#endif
