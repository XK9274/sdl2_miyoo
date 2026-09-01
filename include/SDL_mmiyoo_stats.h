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

/**
 *  \file SDL_mmiyoo_stats.h
 *
 *  Public accessors for the Miyoo mmiyoo renderer's hotpath timing and
 *  geometry instrumentation. Collection must be enabled before renderer
 *  creation via SDL_SetHint(SDL_MMIYOO_FRAME_TIMING / SDL_MMIYOO_GEOMETRY_STATS, "1").
 */

#ifndef SDL_mmiyoo_stats_h_
#define SDL_mmiyoo_stats_h_

#include "SDL_stdinc.h"
#include "SDL_render.h"

#include "begin_code.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_MMIYOO_FrameTimingStats
{
    double fps;
    double cmdqueue_ms_per_frame;
    double present_ms_per_frame;
    double blits_per_frame;
    double fill_ms_per_frame;
    double copy_ms_per_frame;
    double geometry_ms_per_frame;
    double lines_ms_per_frame;
    double misc_ms_per_frame;
} SDL_MMIYOO_FrameTimingStats;

typedef struct SDL_MMIYOO_GeometryStats
{
    Uint64 triangles;
    Uint64 spans;
    Uint64 span_pixels;
    Uint32 max_span_width;
    Uint32 max_span_height;
} SDL_MMIYOO_GeometryStats;

/**
 *  Fills \c out with the most recently completed 1-second frame-timing
 *  window. Returns SDL_FALSE (leaving \c out untouched) if \c renderer isn't
 *  the mmiyoo backend, or SDL_MMIYOO_FRAME_TIMING wasn't set to "1" before
 *  renderer creation.
 */
extern DECLSPEC SDL_bool SDLCALL SDL_MMIYOO_GetFrameTimingStats(SDL_Renderer *renderer, SDL_MMIYOO_FrameTimingStats *out);

/**
 *  Fills \c out with the most recently completed frame's geometry/span
 *  stats. Returns SDL_FALSE (leaving \c out untouched) if \c renderer isn't
 *  the mmiyoo backend, or SDL_MMIYOO_GEOMETRY_STATS wasn't set to "1" before
 *  renderer creation.
 */
extern DECLSPEC SDL_bool SDLCALL SDL_MMIYOO_GetGeometryStats(SDL_Renderer *renderer, SDL_MMIYOO_GeometryStats *out);

#ifdef __cplusplus
}
#endif
#include "close_code.h"

#endif /* SDL_mmiyoo_stats_h_ */
