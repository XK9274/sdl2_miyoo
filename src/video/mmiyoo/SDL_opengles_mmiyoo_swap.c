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

#if SDL_VIDEO_DRIVER_MMIYOO && SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2

#include "SDL_video_mmiyoo.h"
#include "SDL_opengles_mmiyoo.h"
#include "SDL_opengles_mmiyoo_internal.h"
#include <GLES2/gl2.h>

/* Swap interval, pbuffer presentation, windowsurface buffer updates,
 * buffer-settings extension handling, and GLES-to-Miyoo presentation. */

void *ppFunc = NULL;
void *pfb_idx = NULL;
void *pfb_vaddr = NULL;
static SDL_bool g_gles_wait_for_vsync = SDL_TRUE;

static void MMIYOO_GLES_Flip(void)
{
    GFX_SwapBuffers(g_gles_wait_for_vsync);
}

SDL_bool
MMIYOO_GLES_UpdateBufferSettings(_THIS)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    void *fb_vaddr;

    if (!gl_data ||
        gl_data->display == EGL_NO_DISPLAY || gl_data->surface == EGL_NO_SURFACE) {
        return SDL_FALSE;
    }

    fb_vaddr = GFX_GetFrameBufferVirtual();
    if (!fb_vaddr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "MMIYOO GLES: no mapped framebuffer for eglUpdateBufferSettings");
        return SDL_FALSE;
    }

    /* SwiftShader indexes two virtual addresses with fb_idx % 2. Present-copy
     * mode has one stable buffer, so both entries point at it; page-flip mode
     * calls this again after every swap to refresh both to the hidden page. */
    gl_data->fb_idx = 0;
    gl_data->fb_vaddr[0] = (unsigned long)fb_vaddr;
    gl_data->fb_vaddr[1] = (unsigned long)fb_vaddr;

    if (eglUpdateBufferSettings(gl_data->display, gl_data->surface,
                                  (void *)MMIYOO_GLES_Flip,
                                  &gl_data->fb_idx,
                                  gl_data->fb_vaddr) != EGL_TRUE) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                    "MMIYOO GLES: eglUpdateBufferSettings failed (0x%04x)",
                    eglGetError());
        gl_data->buffer_settings_attached = SDL_FALSE;
        return SDL_FALSE;
    }

    GFX_SetBackBufferGLESFormat(SDL_TRUE);
    gl_data->buffer_settings_attached = SDL_TRUE;
    gl_data->owns_buffer_settings = SDL_TRUE;
    return SDL_TRUE;
}

int glSetSwapInterval(_THIS, int interval)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;

    if (!gl_data || gl_data->display == EGL_NO_DISPLAY) {
        return SDL_SetError("MMIYOO: swap interval set without active display");
    }

    if (eglSwapInterval(gl_data->display, interval) != EGL_TRUE) {
        return SDL_SetError("MMIYOO: eglSwapInterval failed (0x%04x)", eglGetError());
    }

    gl_data->swap_interval = interval;
    g_gles_wait_for_vsync = (interval != 0) ? SDL_TRUE : SDL_FALSE;
    return 0;
}

int glGetSwapInterval(_THIS)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    if (!gl_data) {
        return 0;
    }
    return gl_data->swap_interval;
}

int glUpdateBufferSettings(void *pFunc, void *fb_idx, void *fb_vaddr)
{
    ppFunc = pFunc;
    pfb_idx = fb_idx;
    pfb_vaddr = fb_vaddr;
    return 0;
}

static int
MMIYOO_GLES_SwapWindow_PBuffer(_THIS)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    Uint8 *overlay_virt;
    MI_PHY overlay_phy;
    SDL_Rect srcrect, dstrect;
    int pitch;

    overlay_virt = (Uint8 *)GFX_GetOverlayVirtual();
    overlay_phy = GFX_GetOverlayPhysical();
    if (!overlay_virt || !overlay_phy) {
        return SDL_SetError("MMIYOO: no overlay buffer to read GL pixels into");
    }

    pitch = MMIYOO_GLES_RENDER_WIDTH * 4;

    /* glReadPixels() writes directly into gfx.overlay; the GFX_Copy blit
     * below both scales and corrects orientation in hardware. No CPU copy. */
    glReadPixels(0, 0, MMIYOO_GLES_RENDER_WIDTH, MMIYOO_GLES_RENDER_HEIGHT,
                 GL_RGBA, GL_UNSIGNED_BYTE, overlay_virt);

    srcrect.x = 0;
    srcrect.y = 0;
    srcrect.w = MMIYOO_GLES_RENDER_WIDTH;
    srcrect.h = MMIYOO_GLES_RENDER_HEIGHT;

    /* Hardware-scaled present to the full panel; 640/320 and 480/240 are
     * both exact 2x, so this is a clean integer scale. */
    dstrect.x = 0;
    dstrect.y = 0;
    dstrect.w = (int)GFX_GetFrameWidth();
    dstrect.h = (int)GFX_GetFrameHeight();

    /* glReadPixels(GL_RGBA) memory byte order matches E_MI_GFX_FMT_ABGR8888.
     * MIRROR_HORIZONTAL rather than ROTATE_180: the panel's required
     * 180-rotation composed with glReadPixels' bottom-up row order cancels
     * the Y component, leaving a pure X-flip -- done in MI_GFX, not the CPU. */
    if (GFX_Copy(overlay_virt, overlay_phy, srcrect, dstrect, pitch,
                 E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_HORIZONTAL, SDL_BLENDMODE_NONE,
                 NULL, NULL, SDL_FALSE,
                 0, E_MI_GFX_FMT_ABGR8888, 4,
                 255, 255, 255, 255) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "MMIYOO GLES: GFX_Copy present blit failed");
    }

    /* gfx.overlay is single-buffered; flush so next frame's glReadPixels()
     * can't overwrite it while this frame's blit is still reading it. */
    GFX_FlushTextureFences();

    GFX_SwapBuffers(gl_data->swap_interval != 0);

    return 0;
}

int glSwapWindow(_THIS, SDL_Window *window)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    (void)window;

    if (!gl_data || gl_data->display == EGL_NO_DISPLAY || gl_data->surface == EGL_NO_SURFACE) {
        return SDL_SetError("MMIYOO: no EGL surface to swap");
    }

    if (MMIYOO_GLES_ResolvePresentMode() == MMIYOO_GLES_PRESENT_PBUFFER) {
        return MMIYOO_GLES_SwapWindow_PBuffer(_this);
    }

    if (eglSwapBuffers(gl_data->display, gl_data->surface) != EGL_TRUE) {
        return SDL_SetError("MMIYOO: eglSwapBuffers failed (0x%04x)", eglGetError());
    }

    if (gl_data->buffer_settings_attached) {
        if (gl_data->owns_buffer_settings) {
            MMIYOO_GLES_UpdateBufferSettings(_this);
        }
    } else {
        GFX_SwapBuffers(gl_data->swap_interval != 0);
    }

    return 0;
}

#endif /* SDL_VIDEO_DRIVER_MMIYOO && SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2 */
