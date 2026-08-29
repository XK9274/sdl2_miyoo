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

/* Integer upscaling, downscale-composite, and stretch-fill copy paths;
 * NEON scaler selection; scale scratch-buffer lifecycle; rotation/mirror
 * conversion helpers used by scaling and copy. */

MI_GFX_Rotate_e
MMIYOO_AddRotations(MI_GFX_Rotate_e base, MI_GFX_Rotate_e extra)
{
    int total = ((int)base + (int)extra) & 3;
    return (MI_GFX_Rotate_e)total;
}

SDL_bool
MMIYOO_RotationSwapsAxes(MI_GFX_Rotate_e rotation)
{
    return (rotation == E_MI_GFX_ROTATE_90) || (rotation == E_MI_GFX_ROTATE_270);
}

MI_GFX_Mirror_e
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

/* Core-content integer-scale upscaler: MI_GFX_BitBlit has no interpolation
 * control, so scaling happens in software via neon-arm-library-miyoo
 * (github.com/XK9274/neon-arm-library-miyoo). */

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

/* Composites an oversized render-target texture to the screen entirely in
 * hardware: MI_GFX_BitBlit scales whenever source and destination rects
 * differ in size, so one blit does the downscale, rotation, and composite
 * together -- no CPU/NEON pass, no scratch buffer. dst is always the full
 * panel here, already at its own 180-rotation, so no position flip is
 * needed unlike a partial-rect screen draw. */

static SDL_bool
MMIYOO_TryHardwareScaleComposite(MMIYOO_RenderData *data, MMIYOO_TextureData *src_texture_data,
                                  const SDL_Rect *src, int framebuffer_width, int framebuffer_height,
                                  MI_GFX_Rotate_e rotate, MI_GFX_Mirror_e mirror)
{
    MI_GFX_Surface_t src_surf;
    MI_GFX_Rect_t src_rect, dst_rect;
    MI_GFX_Opt_t opt;
    MI_S32 result;
    MI_U16 fence;

    memset(&src_surf, 0, sizeof(src_surf));
    src_surf.phyAddr = src_texture_data->phyAddr;
    src_surf.eColorFmt = src_texture_data->mi_format;
    src_surf.u32Width = (MI_U32)src->w;
    src_surf.u32Height = (MI_U32)src->h;
    src_surf.u32Stride = src_texture_data->pitch;

    src_rect.s32Xpos = src->x;
    src_rect.s32Ypos = src->y;
    src_rect.u32Width = (MI_U32)src->w;
    src_rect.u32Height = (MI_U32)src->h;

    dst_rect.s32Xpos = 0;
    dst_rect.s32Ypos = 0;
    dst_rect.u32Width = (MI_U32)framebuffer_width;
    dst_rect.u32Height = (MI_U32)framebuffer_height;

    memset(&opt, 0, sizeof(opt));
    opt.eRotate = rotate;
    opt.eMirror = mirror;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.stClipRect = dst_rect;

    result = MI_GFX_BitBlit(&src_surf, &src_rect, &data->current_target_surface, &dst_rect, &opt, &fence);
    if (result != MI_SUCCESS) {
        MMIYOO_LOG_WARN("MMIYOO_TryHardwareScaleComposite: MI_GFX_BitBlit failed (result=0x%x), falling back to NEON",
                        result);
        return SDL_FALSE;
    }
    GFX_AddTextureFence(fence);
    return SDL_TRUE;
}

/* Tries the hardware scale above first; falls back to the NEON downscale
 * into a framebuffer-sized scratch buffer if that fails. *handled_directly
 * is set when the hardware path already completed the composite itself --
 * the caller must not issue its own GFX_Copy in that case. Returns
 * SDL_FALSE only when the input is unsupported by either path. */

SDL_bool
MMIYOO_TryDownscaleCompositeCopy(MMIYOO_RenderData *data, SDL_Texture *texture,
                                  MMIYOO_TextureData *src_texture_data,
                                  SDL_Rect *src, SDL_Rect *dst,
                                  SDL_BlendMode blend_mode,
                                  MI_GFX_Rotate_e rotate, MI_GFX_Mirror_e mirror,
                                  const void **pixels, int *pitch, MI_PHY *src_phy,
                                  SDL_bool *handled_directly)
{
    int framebuffer_width;
    int framebuffer_height;
    unsigned int dst_stride;
    unsigned int required_size;
    unsigned int bpp;
    MMIYOO_NeonDownscaleFunc downscale_func;

    (void)texture;
    *handled_directly = SDL_FALSE;

    if (blend_mode != SDL_BLENDMODE_NONE) {
        return SDL_FALSE;
    }
    if (src->w <= 0 || src->h <= 0) {
        return SDL_FALSE;
    }

    framebuffer_width = MMIYOO_GetFramebufferWidth(data);
    framebuffer_height = MMIYOO_GetFramebufferHeight(data);
    if (framebuffer_width <= 0 || framebuffer_height <= 0) {
        return SDL_FALSE;
    }

    /* Drawing into this render target only queues GFX fences without
     * waiting; the NEON fallback reads it on the CPU, so flush first
     * regardless of which path below ends up running. */
    GFX_FlushTextureFences();

    if (MMIYOO_TryHardwareScaleComposite(data, src_texture_data, src, framebuffer_width, framebuffer_height,
                                          rotate, mirror)) {
        *handled_directly = SDL_TRUE;
        return SDL_TRUE;
    }

    bpp = src_texture_data->bytes_per_pixel;
    downscale_func = MMIYOO_PickDownscaleFunc(bpp);

    if (!downscale_func) {
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

SDL_bool
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

void
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

#endif /* SDL_VIDEO_RENDER_MMIYOO */
