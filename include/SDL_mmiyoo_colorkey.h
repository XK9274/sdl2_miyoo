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
 *  \file SDL_mmiyoo_colorkey.h
 *
 *  Hardware source colorkey for the Miyoo mmiyoo renderer. SDL itself has
 *  no vtable hook to set a colorkey on an existing SDL_Texture -- the
 *  built-in colorkey support in SDL_CreateTextureFromSurface already
 *  converts a surface's colorkey into per-pixel alpha=0 upstream of any
 *  renderer, which does not help a texture kept in an alpha-less format
 *  (e.g. RGB565) that still needs a color keyed out.
 *
 *  The underlying MI_GFX source colorkey has no usable effect on this
 *  hardware -- a silicon/firmware limitation, not a wiring bug. This
 *  function currently has no visible effect regardless of its return value.
 */

#ifndef SDL_mmiyoo_colorkey_h_
#define SDL_mmiyoo_colorkey_h_

#include "SDL_stdinc.h"
#include "SDL_render.h"

#include "begin_code.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 *  Enables or disables hardware source colorkey on \c texture. \c key is a
 *  format-agnostic (r<<16)|(g<<8)|b triplet; pixels matching it are treated
 *  as transparent when this texture is copied. Returns SDL_FALSE if
 *  \c texture doesn't belong to the mmiyoo renderer.
 *
 *  Currently has no visible effect on real hardware even when it returns
 *  SDL_TRUE.
 */
extern DECLSPEC SDL_bool SDLCALL SDL_MMIYOO_SetTextureColorKey(SDL_Texture *texture, SDL_bool enabled, Uint32 key);

#ifdef __cplusplus
}
#endif
#include "close_code.h"

#endif /* SDL_mmiyoo_colorkey_h_ */
