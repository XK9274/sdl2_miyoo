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

/* SDL-to-MI pixel-format conversion, texture frame-data conversion, MMA
 * allocation/mapping, the bounded texture-memory reuse pool, texture
 * create/lock/update/unlock/scale-mode/destroy, and the CPU/GFX
 * cache-coherency helper used at texture memory boundaries. */

static int mmiyoo_texture_live_count = 0;

void
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

/* TODO: rework this pool -- bucketed linear-scan reuse over a fixed-size
 * array is a stopgap, not a real allocator design. */

/* Bounded, size-bucketed reuse pool for MI_SYS_MMA texture memory blocks.
 * Freed blocks are cached here instead of freed immediately, and reused by
 * a later CreateTexture of a compatible size. Hard count/byte caps bound
 * it to no worse than no pool at all. Safe to reuse a cached block without
 * extra synchronization since MMIYOO_DestroyTexture already flushes all
 * GFX fences before a block reaches this pool. */
typedef struct {
    MI_PHY phyAddr;
    void *virAddr;
    unsigned int alloc_size;
} MMIYOO_PooledBlock;

#define MMIYOO_TEXTURE_POOL_MAX_ENTRIES 24
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

/* Called once from MMIYOO_CreateRenderer's hint parsing; keeps the pool's
 * state private to this file instead of exposing raw shared globals. */
void
MMIYOO_TexturePoolConfigure(SDL_bool enabled, size_t max_bytes)
{
    mmiyoo_texture_pool_enabled = enabled;
    mmiyoo_texture_pool_max_bytes = (unsigned int)max_bytes;
}

static MI_SYS_PixelFormat_e MMIYOO_SDLToMISysFormat(Uint32 sdl_format) {
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

static int MMIYOO_TextureToFrameData(MMIYOO_TextureData *texture, MI_SYS_FrameData_t *frame_data) {
    MI_SYS_PixelFormat_e sys_format;
    
    if (!texture || !frame_data) return -1;
    
    sys_format = MMIYOO_SDLToMISysFormat(texture->format);
    
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

// DMA-driven MI_SYS-to-MI_SYS blit via MI_SYS_BufBlitPa
int MMIYOO_DMABlitTextureToTexture(MMIYOO_TextureData *src_texture, SDL_Rect *src_rect,
                                       MMIYOO_TextureData *dst_texture, SDL_Rect *dst_rect) {
    MI_SYS_FrameData_t src_frame, dst_frame;
    MI_SYS_WindowRect_t mi_src_rect, mi_dst_rect;
    
    if (MMIYOO_TextureToFrameData(src_texture, &src_frame) != 0) return -1;
    if (MMIYOO_TextureToFrameData(dst_texture, &dst_frame) != 0) return -1;
    
    mi_src_rect.u16X = src_rect ? src_rect->x : 0;
    mi_src_rect.u16Y = src_rect ? src_rect->y : 0;
    mi_src_rect.u16Width = src_rect ? src_rect->w : src_texture->width;
    mi_src_rect.u16Height = src_rect ? src_rect->h : src_texture->height;

    mi_dst_rect.u16X = dst_rect ? dst_rect->x : 0;
    mi_dst_rect.u16Y = dst_rect ? dst_rect->y : 0;
    mi_dst_rect.u16Width = dst_rect ? dst_rect->w : dst_texture->width;
    mi_dst_rect.u16Height = dst_rect ? dst_rect->h : dst_texture->height;

    return MI_SYS_BufBlitPa(&dst_frame, &mi_dst_rect, &src_frame, &mi_src_rect);
}

static MI_GFX_ColorFmt_e MMIYOO_SDLToMIGfxFormat(Uint32 sdl_format, int *bits_per_pixel, const char **format_name) {
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
            /* No MI_GFX format matches RGBA8888's real memory order (A,B,G,R); this swaps R/B. */
            return E_MI_GFX_FMT_ARGB8888;
            
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
MMIYOO_TexturePoolTryAcquire(unsigned int requested_size, MI_PHY *out_phy, void **out_vir,
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
MMIYOO_TexturePoolReleaseOrFree(MI_PHY phyAddr, void *virAddr, unsigned int alloc_size)
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

void
MMIYOO_TexturePoolDrain(void)
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

int MMIYOO_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
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
        mi_format = MMIYOO_SDLToMIGfxFormat(texture->format, &bits_temp, &format_name);
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
        MMIYOO_TexturePoolTryAcquire(mmiyoo_texture->size, &mmiyoo_texture->phyAddr,
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
                MMIYOO_TexturePoolDrain();
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

int MMIYOO_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    MMIYOO_TextureData *mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;

    *pixels = mmiyoo_texture->data;
    *pitch = mmiyoo_texture->pitch;
    return 0;
}

int MMIYOO_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
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

void MMIYOO_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_Rect rect = {0};
    MMIYOO_TextureData *mmiyoo_texture = (MMIYOO_TextureData*)texture->driverdata;

    rect.x = 0;
    rect.y = 0;
    rect.w = texture->w;
    rect.h = texture->h;
    MMIYOO_UpdateTexture(renderer, texture, &rect, mmiyoo_texture->data, mmiyoo_texture->pitch);
}

void MMIYOO_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
}

void MMIYOO_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
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
                MMIYOO_TexturePoolReleaseOrFree(mmiyoo_texture->phyAddr, mmiyoo_texture->virAddr,
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

#endif /* SDL_VIDEO_RENDER_MMIYOO */
