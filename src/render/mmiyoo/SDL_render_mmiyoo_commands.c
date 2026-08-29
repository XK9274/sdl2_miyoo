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

/* SDL command-queue producers, queue execution, geometry-texture command
 * handling, and the copy-command executor. The orchestrator: delegates to
 * geometry.c's primitives, texture.c's conversion/pool, and scaling.c's
 * copy paths. Owns line/rect batching, since that's queue-execution
 * strategy rather than geometry math. */

SDL_bool g_warned_copyex_angle = SDL_FALSE;

/* Forward declaration: MMIYOO_LineBatchAccumulate calls this before its own
 * definition appears further down the file. */
static void MMIYOO_LineBatchFlush(SDL_Renderer *renderer, MMIYOO_RenderData *data, MMIYOO_LineBatch *batch, Uint32 color);

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

void
MMIYOO_UpdateClipState(MMIYOO_RenderData *data, SDL_bool enabled, const SDL_Rect *rect)
{
    data->clip_enabled = enabled;
    if (enabled && rect) {
        data->clip_rect = *rect;
    } else {
        SDL_zero(data->clip_rect);
    }
}

int MMIYOO_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
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

int MMIYOO_QueueSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
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
int MMIYOO_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count);

int MMIYOO_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
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

int MMIYOO_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
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

int MMIYOO_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
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

int MMIYOO_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count)
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

int MMIYOO_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_FRect *dstrect)
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

static int MMIYOO_ExecuteCopyCommand(SDL_Renderer *renderer,
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
            /* Oversized render-target texture composited to the screen:
             * MMIYOO_TryDownscaleCompositeCopy tries a single hardware
             * MI_GFX_BitBlit scale first, falling back to NEON only if
             * that fails. */
            SDL_bool handled_directly = SDL_FALSE;
            used_downscale = MMIYOO_TryDownscaleCompositeCopy(data, texture, src_texture_data, &src, &dst,
                                                                blend_mode, effective_rotation, mirror,
                                                                &pixels, &pitch, &src_phy, &handled_directly);
            if (!used_downscale) {
                /* Degenerate/unsupported input already logged once inside
                 * MMIYOO_TryDownscaleCompositeCopy -- never fall through to
                 * GFX_Copy with the untouched oversized source. */
                return 0;
            }
            if (handled_directly) {
                /* Hardware path already issued and fenced its own BitBlit;
                 * nothing left for the GFX_Copy call below to do. */
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
            if (MMIYOO_DMABlitTextureToTexture(src_texture_data, &src, dst_texture_data, &dst) == MI_SUCCESS) {
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

    /* Mark that a render-target texture was blitted to screen this frame. */
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

int MMIYOO_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
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

            /* 1x1 opaque source texture: fill with QuickFill instead of a
             * texture blit. Opaque-only since MI_GFX_QuickFill has no blend
             * flags. Uses the triangle's bounding box, not exact coverage,
             * so a non-rectangular triangle can be over-painted slightly. */
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

            if (MMIYOO_ExecuteCopyCommand(renderer, cmd->data.draw.texture, NULL,
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

int MMIYOO_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
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

                if (MMIYOO_ExecuteCopyCommand(renderer, texture, pixels, &src, &dstf, cmd->data.draw.blend,
                                  E_MI_GFX_ROTATE_0, SDL_FLIP_NONE, default_center,
                                  cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a) != 0) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY: MMIYOO_ExecuteCopyCommand failed");
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

                if (MMIYOO_ExecuteCopyCommand(renderer, texture, NULL, &copydata->srcrect, &dstf, cmd->data.draw.blend,
                                  extra_rotation, copydata->flip, copydata->center,
                                  cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a) != 0) {
                    MMIYOO_LOG_WARN("RunCommandQueue COPY_EX: MMIYOO_ExecuteCopyCommand failed");
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

#endif /* SDL_VIDEO_RENDER_MMIYOO */
