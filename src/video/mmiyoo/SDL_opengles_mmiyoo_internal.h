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
#ifndef SDL_opengles_mmiyoo_internal_h_
#define SDL_opengles_mmiyoo_internal_h_

/* Cross-file contract between SDL_opengles_mmiyoo_context.c and _swap.c. */

/* GL render target size. Must match what the GL client renders at. Full
 * panel resolution needs no hardware scale-up (srcrect == dstrect in
 * glSwapWindow) but costs a full-panel SwiftShader software render every
 * frame; drop to e.g. 320x240 to trade render cost for a hardware upscale
 * on present. */
#define MMIYOO_GLES_RENDER_WIDTH  640
#define MMIYOO_GLES_RENDER_HEIGHT 480

/* Present strategies, selected via SDL_MMIYOO_GLES_PRESENT_MODE:
 *   "pbuffer" (default) -- EGL PBuffer, glReadPixels() into gfx.overlay,
 *     hardware-blit to the panel. Colour-correct, SwiftShader-limited.
 *   "windowsurface" -- EGL WindowSurface via the vendor
 *     eglUpdateBufferSettings extension into gfx.back. Comparison/
 *     regression only: known colour corruption in translucent UI regions,
 *     root cause not found -- see TODO. */
typedef enum {
    MMIYOO_GLES_PRESENT_PBUFFER = 0,
    MMIYOO_GLES_PRESENT_WINDOWSURFACE
} MMIYOO_GLESPresentMode_e;

/* Defined in _context.c; _swap.c calls this instead of re-resolving the
 * SDL_MMIYOO_GLES_PRESENT_MODE env var itself. */
MMIYOO_GLESPresentMode_e MMIYOO_GLES_ResolvePresentMode(void);

/* windowsurface mode only. Exported by the vendor libEGL.so but absent
 * from its eglGetProcAddress table, so it must be linked directly. */
extern EGLBoolean eglUpdateBufferSettings(EGLDisplay display, EGLSurface surface, void *pFunc, void *fb_idx, void *fb_vaddr);

/* Vendor buffer-settings state. _swap.c's MMIYOO_GLES_UpdateBufferSettings
 * can run before any GL context exists, stashing these; _context.c's
 * glCreateContext reads them back to re-attach settings to a freshly
 * created windowsurface context. */
extern void *ppFunc;
extern void *pfb_idx;
extern void *pfb_vaddr;

/* Also called directly from _context.c's glCreateContext (windowsurface
 * branch) to re-attach settings registered before the context existed. */
SDL_bool MMIYOO_GLES_UpdateBufferSettings(_THIS);

#endif /* SDL_opengles_mmiyoo_internal_h_ */
