/*
  Simple DirectMedia Layer
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
#include "SDL.h"

#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>

#include "SDL_hints.h"
#include "SDL_pixels.h"
#include "SDL_mmiyoo.h"

/* VSync-hint resolution and framebuffer-info probing, used by the video
 * device and (for present-vsync resolution) the renderer. */

MMIYOO_VSyncMode_e
MMIYOO_GetVSyncMode(void)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_VSYNC_MODE);
    if (mode && SDL_strcmp(mode, "adaptive") == 0) {
        return MMIYOO_VSYNC_MODE_ADAPTIVE;
    }
    if (mode && SDL_strcmp(mode, "strict") == 0) {
        return MMIYOO_VSYNC_MODE_STRICT;
    }
    /* Default is "off" - see SDL_HINT_MMIYOO_VSYNC_MODE comment in SDL_mmiyoo.h. */
    return MMIYOO_VSYNC_MODE_OFF;
}

MMIYOO_VSyncMode_e
MMIYOO_ResolvePresentVSyncMode(SDL_bool renderer_vsync_requested)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_VSYNC_MODE);
    if (mode && *mode) {
        return MMIYOO_GetVSyncMode();
    }
    return renderer_vsync_requested ? MMIYOO_VSYNC_MODE_ADAPTIVE : MMIYOO_VSYNC_MODE_OFF;
}

void
MMIYOO_GetDefaultFramebufferInfo(MMIYOO_FramebufferInfo *info)
{
    if (!info) {
        return;
    }

    info->width = MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
    info->height = MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
    info->bytes_per_pixel = MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL;
    info->stride = info->width * info->bytes_per_pixel;
    info->active_size = info->stride * info->height;
    info->sdl_format = SDL_PIXELFORMAT_ARGB8888;
}

SDL_bool
MMIYOO_GetFramebufferInfoFromFD(int fb_fd, MMIYOO_FramebufferInfo *info)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    SDL_bool have_vinfo;
    SDL_bool have_finfo;

    if (!info) {
        return SDL_FALSE;
    }

    MMIYOO_GetDefaultFramebufferInfo(info);

    if (fb_fd < 0) {
        return SDL_FALSE;
    }

    SDL_zero(vinfo);
    SDL_zero(finfo);
    have_vinfo = (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) ? SDL_TRUE : SDL_FALSE;
    have_finfo = (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) ? SDL_TRUE : SDL_FALSE;

    if (!have_vinfo && !have_finfo) {
        return SDL_FALSE;
    }

    if (have_vinfo) {
        if (vinfo.xres > 0) {
            info->width = (int)vinfo.xres;
        }
        if (vinfo.yres > 0) {
            info->height = (int)vinfo.yres;
        }
        if (vinfo.bits_per_pixel > 0) {
            info->bytes_per_pixel = (int)((vinfo.bits_per_pixel + 7) / 8);
            if (info->bytes_per_pixel <= 0) {
                info->bytes_per_pixel = MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL;
            }
            info->sdl_format = (vinfo.bits_per_pixel == 16) ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB8888;
        }
    }

    if (have_finfo && finfo.line_length > 0) {
        info->stride = (int)finfo.line_length;
    } else {
        info->stride = info->width * info->bytes_per_pixel;
    }

    info->active_size = info->stride * info->height;
    if (info->active_size <= 0) {
        info->active_size = info->width * info->height * info->bytes_per_pixel;
    }

    return SDL_TRUE;
}

/* vi: set ts=4 sw=4 expandtab: */
