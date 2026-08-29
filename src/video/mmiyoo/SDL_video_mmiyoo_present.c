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

/* Buffer swap/page-flip logic, framebuffer/overlay accessors, GLES
 * back-buffer format state, and off/adaptive/strict presentation pacing. */

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

SDL_bool GFX_IsPageFlipEnabled(void)
{
#ifdef MMIYOO
    return gfx.page_flip_enabled;
#else
    return SDL_FALSE;
#endif
}

void GFX_SwapBuffers(SDL_bool wait_for_vsync)
{
#ifdef MMIYOO
    MI_U32 copy_bytes;
    MI_U32 frame_bytes;
    /* SDL_MMIYOO_VSYNC_MODE wins if explicitly set; otherwise the standard
     * SDL renderer vsync request (wait_for_vsync) decides adaptive vs off. */
    const MMIYOO_VSyncMode_e vsync_mode = MMIYOO_ResolvePresentVSyncMode(wait_for_vsync);

    if (!gfx.double_buffer_enabled || gfx.back.phyAddr == 0 || gfx.fb.phyAddr == 0) {
        return;
    }

    if (vsync_mode != MMIYOO_VSYNC_MODE_OFF && gfx.fb_dev > 0 && !gfx.page_flip_enabled) {
        static SDL_bool vsync_unsupported_warned = SDL_FALSE;
        static Uint64 last_present_ticks = 0;
        const Uint64 target_interval_ms = 17; /* one frame @ 60Hz */
        const Uint64 now = SDL_GetTicks64();
        const SDL_bool already_late = (vsync_mode == MMIYOO_VSYNC_MODE_ADAPTIVE)
            && last_present_ticks != 0
            && (now - last_present_ticks) >= target_interval_ms;

        if (!already_late) {
            __u32 crtc = 0;

            if (ioctl(gfx.fb_dev, FBIO_WAITFORVSYNC, &crtc) != 0 && !vsync_unsupported_warned) {
                MMIYOO_LOG_WARN("GFX_SwapBuffers: FBIO_WAITFORVSYNC not supported, presenting unsynchronized");
                vsync_unsupported_warned = SDL_TRUE;
            }
        }

        last_present_ticks = SDL_GetTicks64();
    }

    if (gfx.page_flip_enabled) {
        /* Real flip: pan the CRTC to the half we just rendered, no copy.
         * page_flip_index tracks which half is the front (scanned-out) one;
         * flip it, pan to it, then point gfx.back at the now-hidden half. */
        frame_bytes = gfx.back.length;
        if (gfx.back.virAddr) {
            MI_SYS_FlushInvCache(gfx.back.virAddr, frame_bytes);
        }

        gfx.page_flip_index ^= 1;
        gfx.vinfo.yoffset = gfx.page_flip_index ? gfx.vinfo.yres : 0;
        /* /dev/l handles panning in strict-mode vsync: 60fps normally, hard-steps to 30fps under load. Killing /dev/l regains control but introduces flickering. */
        ioctl(gfx.fb_dev, FBIOPAN_DISPLAY, &gfx.vinfo);

        gfx.back.phyAddr = gfx.fb.phyAddr + (gfx.page_flip_index ? 0 : frame_bytes);
        gfx.back.virAddr = (Uint8 *)gfx.fb.virAddr + (gfx.page_flip_index ? 0 : frame_bytes);
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

    /* Not used for the present-copy: MI_SYS_MemcpyPa has no completion fence,
     * so the next frame could start overwriting gfx.back before the DMA
     * actually finished (measured ~0.008ms return vs ~1.2-4ms real transfer
     * time). A fixed/adaptive delay can't safely replace the fence here --
     * the whole MI_SYS Memcpy/BufFillPa/BufBlitPa family exposes no
     * completion signal to size or gate one on, so any delay is a guess:
     * too short and the race just gets narrower, too long and it wastes
     * frame budget every frame. MI_GFX_BitBlit's real fence waits exactly
     * as long as needed either way. Kept only as a reference; MemcpyPa is
     * still used elsewhere for buffer ops where that's not a concern. */
    /*
    if (MI_SYS_MemcpyPa(gfx.fb.phyAddr, gfx.back.phyAddr, copy_bytes) != MI_SUCCESS) {
        MMIYOO_LOG_WARN("GFX_SwapBuffers: MI_SYS_MemcpyPa failed (bytes=%u)", copy_bytes);
    }
    */
    {
        MI_GFX_Surface_t src_surf;
        MI_GFX_Surface_t dst_surf;
        MI_GFX_Rect_t src_rect;
        MI_GFX_Rect_t dst_rect;
        MI_GFX_Opt_t opt;
        MI_U16 fence = 0;
        MI_S32 result;

        memset(&src_surf, 0, sizeof(src_surf));
        src_surf.phyAddr = gfx.back.phyAddr;
        src_surf.eColorFmt = gfx.back_buffer_is_gles_rgba ? E_MI_GFX_FMT_ABGR8888 : E_MI_GFX_FMT_ARGB8888;
        src_surf.u32Width = GFX_GetFrameWidth();
        src_surf.u32Height = GFX_GetFrameHeight();
        src_surf.u32Stride = GFX_GetFrameStride();

        dst_surf = src_surf;
        dst_surf.phyAddr = gfx.fb.phyAddr;
        dst_surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;

        memset(&src_rect, 0, sizeof(src_rect));
        src_rect.u32Width = src_surf.u32Width;
        src_rect.u32Height = src_surf.u32Height;
        dst_rect = src_rect;

        memset(&opt, 0, sizeof(opt));
        opt.eRotate = E_MI_GFX_ROTATE_0;
        opt.eMirror = E_MI_GFX_MIRROR_NONE;
        opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
        /* Straight opaque copy: src*ONE + dst*ZERO. The memset zero default (ZERO/ZERO) instead computes a solid black frame regardless of eDFBBlendFlag. */
        opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
        opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
        opt.stClipRect.s32Xpos = 0;
        opt.stClipRect.s32Ypos = 0;
        opt.stClipRect.u32Width = src_surf.u32Width;
        opt.stClipRect.u32Height = src_surf.u32Height;

        result = MI_GFX_BitBlit(&src_surf, &src_rect, &dst_surf, &dst_rect, &opt, &fence);
        if (result == MI_SUCCESS) {
            MI_GFX_WaitAllDone(FALSE, fence);
        } else {
            MMIYOO_LOG_WARN("GFX_SwapBuffers: MI_GFX_BitBlit present-copy failed (result=%d)", result);
        }
    }
#endif
}

void GFX_SetBackBufferGLESFormat(SDL_bool is_gles_rgba)
{
#ifdef MMIYOO
    gfx.back_buffer_is_gles_rgba = is_gles_rgba;
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

void *GFX_GetOverlayVirtual(void)
{
#ifdef MMIYOO
    return gfx.overlay.virAddr;
#else
    return NULL;
#endif
}

MI_PHY GFX_GetOverlayPhysical(void)
{
#ifdef MMIYOO
    return gfx.overlay.phyAddr;
#else
    return 0;
#endif
}

#endif /* SDL_VIDEO_DRIVER_MMIYOO */
