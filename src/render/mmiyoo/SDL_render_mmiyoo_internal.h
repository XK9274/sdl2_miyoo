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
#ifndef SDL_render_mmiyoo_internal_h_
#define SDL_render_mmiyoo_internal_h_

/* Private cross-file contract for the mmiyoo renderer's translation units
 * (SDL_render_mmiyoo.c, _geometry.c, _texture.c, _commands.c, _scaling.c,
 * _present.c). */

#include "SDL_rect.h"
#include "SDL_render.h"
#include "../SDL_sysrender.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"

#define MMIYOO_SYS_ALIGNMENT 4096u
#define MMIYOO_ALIGN_SYS(value) (((value) + MMIYOO_SYS_ALIGNMENT - 1u) & ~(MMIYOO_SYS_ALIGNMENT - 1u))

/* Shared with top-level's SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES hint-parsing default. */
#define MMIYOO_TEXTURE_POOL_DEFAULT_MAX_BYTES (10u * 1024u * 1024u)

/* Set once from SDL_MMIYOO_DEBUG/_VERBOSE hints in MMIYOO_CreateRenderer (SDL_render_mmiyoo.c);
 * read from texture.c, present.c, and top-level via MMIYOO_VERBOSE_LOG. */
extern SDL_bool mmiyoo_debug_verbose;

/* Latched COPY_EX angle-support warning; reset in MMIYOO_CreateRenderer, set/read in commands.c. */
extern SDL_bool g_warned_copyex_angle;

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

typedef struct MMIYOO_RenderData {
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
     * default -- trades a little text crispness for fewer hardware blits. */
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
} MMIYOO_RenderData;

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

/* One bounding-box blit per triangle, not one for the whole geometry call.
 * tri_count MMIYOO_GeometryTextureTri entries follow this header in the
 * allocated vertex buffer. */
typedef struct {
    MI_GFX_Surface_t target_surface;
    SDL_bool is_target_texture;
    int tri_count;
} MMIYOO_GeometryTextureData;

/* --- geometry.c public API --- */
SDL_bool MMIYOO_ClipLineToRect(float *x0, float *y0, float *x1, float *y1, const SDL_Rect *rect);
void MMIYOO_DrawFilledTriangle(MMIYOO_RenderData *data,
                               const SDL_FPoint *p0,
                               const SDL_FPoint *p1,
                               const SDL_FPoint *p2,
                               const SDL_Rect *clip_rect,
                               Uint32 color);
Uint32 MMIYOO_PackColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int MMIYOO_FloatToPixel(float value);
void MMIYOO_ApplyViewportToPoint(const MMIYOO_RenderData *data, float *x, float *y);
int MMIYOO_GetFramebufferWidth(const MMIYOO_RenderData *data);
int MMIYOO_GetFramebufferHeight(const MMIYOO_RenderData *data);
SDL_Rect MMIYOO_GetTargetBounds(const MMIYOO_RenderData *data);
SDL_bool MMIYOO_PrepareDrawRect(SDL_Renderer *renderer,
                                MMIYOO_RenderData *data,
                                SDL_Rect *dst,
                                SDL_Rect *src,
                                SDL_Rect *out_clip,
                                SDL_bool *clip_enabled);
void MMIYOO_ExecuteQuickFill(MMIYOO_RenderData *data, const SDL_Rect *dst, Uint32 color);
SDL_bool MMIYOO_ExecuteDrawLine(MMIYOO_RenderData *data, float x0, float y0, float x1, float y1, Uint32 color);

/* --- commands.c public API (also wired directly into the SDL_Renderer vtable by
 * MMIYOO_CreateRenderer in the top-level file) --- */
void MMIYOO_UpdateClipState(MMIYOO_RenderData *data, SDL_bool enabled, const SDL_Rect *rect);
int MMIYOO_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd);
int MMIYOO_QueueSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd);
int MMIYOO_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count);
int MMIYOO_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count);
int MMIYOO_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                         const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
                         int num_vertices, const void *indices, int num_indices, int size_indices,
                         float scale_x, float scale_y);
int MMIYOO_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count);
int MMIYOO_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_FRect *dstrect);
int MMIYOO_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                       const SDL_Rect *srcrect, const SDL_FRect *dstrect,
                       const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip);
int MMIYOO_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize);

/* --- texture.c public API (CreateTexture/LockTexture/UpdateTexture/UnlockTexture/
 * SetTextureScaleMode/DestroyTexture are also wired into the SDL_Renderer vtable by
 * MMIYOO_CreateRenderer) --- */
void MMIYOO_FlushInvCacheRange(void *address, size_t size);
void MMIYOO_TexturePoolConfigure(SDL_bool enabled, size_t max_bytes);
void MMIYOO_TexturePoolDrain(void);
int MMIYOO_DMABlitTextureToTexture(MMIYOO_TextureData *src_texture, SDL_Rect *src_rect,
                                   MMIYOO_TextureData *dst_texture, SDL_Rect *dst_rect);
int MMIYOO_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture);
int MMIYOO_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch);
int MMIYOO_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
void MMIYOO_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture);
void MMIYOO_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode);
void MMIYOO_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture);

/* --- scaling.c public API --- */
MI_GFX_Rotate_e MMIYOO_AddRotations(MI_GFX_Rotate_e base, MI_GFX_Rotate_e extra);
SDL_bool MMIYOO_RotationSwapsAxes(MI_GFX_Rotate_e rotation);
MI_GFX_Mirror_e MMIYOO_FlipToMirror(SDL_RendererFlip flip);
SDL_bool MMIYOO_TryDownscaleCompositeCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                                          MMIYOO_TextureData *src_texture_data,
                                          SDL_Rect *src, SDL_Rect *dst,
                                          SDL_BlendMode blend_mode,
                                          MI_GFX_Rotate_e rotate, MI_GFX_Mirror_e mirror,
                                          const void **pixels, int *pitch, MI_PHY *src_phy,
                                          SDL_bool *handled_directly);
SDL_bool MMIYOO_TryIntegerScaleCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                                    MMIYOO_TextureData *src_texture_data,
                                    SDL_Rect *src, SDL_Rect *dst,
                                    SDL_BlendMode blend_mode,
                                    const void **pixels, int *pitch, MI_PHY *src_phy);
void MMIYOO_TryStretchFillCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                               SDL_Rect *src, SDL_Rect *dst, SDL_BlendMode blend_mode);

/* --- present.c public API (RenderReadPixels/RenderPresent/SetVSync are also wired
 * into the SDL_Renderer vtable by MMIYOO_CreateRenderer) --- */
void MMIYOO_UpdatePresentVSyncFlag(SDL_Renderer *renderer, SDL_bool renderer_vsync_requested);
int MMIYOO_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 pixel_format, void *pixels, int pitch);
void MMIYOO_RenderPresent(SDL_Renderer *renderer);
int MMIYOO_SetVSync(SDL_Renderer *renderer, const int vsync);

#endif /* SDL_render_mmiyoo_internal_h_ */
