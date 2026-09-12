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

#if SDL_VIDEO_RENDER_MMIYOO || SDL_VIDEO_DRIVER_MMIYOO

#include "SDL_stdinc.h"
#include "../../video/mmiyoo/SDL_video_mmiyoo.h"
#include "SDL_mmiyoo_pixelformat.h"

/* Single SDL-to-MI_GFX format table shared by the render texture layer and
 * GFX_Copy, so the two never drift out of sync. */
MI_GFX_ColorFmt_e MMIYOO_SDLToMIGfxFormat(Uint32 sdl_format, int *bits_per_pixel, const char **format_name)
{
    switch (sdl_format) {
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
            /* TODO: dev-tools/pixel-format-probe (mm-buildbot) shows this decodes as if it were ARGB8888 -- needs investigation. */
            return E_MI_GFX_FMT_ABGR8888;

        case SDL_PIXELFORMAT_BGRA8888:
            *bits_per_pixel = 32;
            *format_name = "BGRA8888";
            /* TODO: also decodes as if ARGB8888, same as ABGR8888 above -- needs investigation. */
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

#endif /* SDL_VIDEO_RENDER_MMIYOO || SDL_VIDEO_DRIVER_MMIYOO */

/* vi: set ts=4 sw=4 expandtab: */
