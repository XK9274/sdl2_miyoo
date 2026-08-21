/*
  Customized version for Miyoo-Mini handheld.
  Only tested under Miyoo-Mini stock OS (original firmware) with Parasyte compatible layer.

  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2022-2022 Steward Fu <steward.fu@gmail.com>

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
#include <GLES2/gl2.h>

/* GL render target size, in lockstep with RetroArch's own
 * video_fullscreen_x/y in retroarch.cfg -- they must match exactly, or RA
 * lays out its UI for a canvas size that doesn't match what we actually
 * give it (the "quarter screen" bug: RA rendered crisply at 320x240 while
 * believing the target was 640x480). Currently full real panel resolution
 * -- no hardware scale-up needed (srcrect == dstrect in glSwapWindow), at
 * the cost of SwiftShader software-rendering the full panel every frame.
 * Drop to a smaller size (e.g. 320x240, an exact 2x-downscale of this
 * panel) to trade render cost for a clean integer hardware upscale on
 * present if full-res proves too slow again -- same shape every
 * offscreen-FBO suite in miyoo_sdl2_benchmarks already uses. */
#define MMIYOO_GLES_RENDER_WIDTH  640
#define MMIYOO_GLES_RENDER_HEIGHT 480

/* Two present strategies, selected via SDL_MMIYOO_GLES_PRESENT_MODE:
 *   "pbuffer" (default) -- render into an EGL PBuffer, glReadPixels() into
 *     gfx.overlay, hardware-blit that to the panel. Colour-correct, but
 *     still bound by SwiftShader's software-rasterizer performance ceiling.
 *   "windowsurface" -- a real EGL WindowSurface, presented through the
 *     vendor eglUpdateBufferSettings extension into gfx.back. Kept for
 *     comparison/regression testing only: known to still produce colour
 *     corruption (green patches) in translucent UI regions, root cause not
 *     found -- see TODO. */
typedef enum {
    MMIYOO_GLES_PRESENT_PBUFFER = 0,
    MMIYOO_GLES_PRESENT_WINDOWSURFACE
} MMIYOO_GLESPresentMode_e;

static MMIYOO_GLESPresentMode_e g_present_mode = MMIYOO_GLES_PRESENT_PBUFFER;
static SDL_bool g_present_mode_resolved = SDL_FALSE;

static MMIYOO_GLESPresentMode_e
MMIYOO_GLES_ResolvePresentMode(void)
{
    if (!g_present_mode_resolved) {
        const char *env = SDL_getenv("SDL_MMIYOO_GLES_PRESENT_MODE");
        g_present_mode = (env && SDL_strcasecmp(env, "windowsurface") == 0)
                          ? MMIYOO_GLES_PRESENT_WINDOWSURFACE
                          : MMIYOO_GLES_PRESENT_PBUFFER;
        g_present_mode_resolved = SDL_TRUE;
    }
    return g_present_mode;
}

/* windowsurface mode only, below: exported by the vendor libEGL.so but not
 * registered in its eglGetProcAddress extension table, so it must be linked
 * directly rather than looked up at runtime -- eglGetProcAddress
 * ("eglUpdateBufferSettings") reliably returns NULL on-device even though
 * the symbol is present in the .so. */
extern EGLBoolean eglUpdateBufferSettings(EGLDisplay display, EGLSurface surface, void *pFunc, void *fb_idx, void *fb_vaddr);

static void *ppFunc = NULL;
static void *pfb_idx = NULL;
static void *pfb_vaddr = NULL;
static SDL_bool g_gles_wait_for_vsync = SDL_TRUE;

static void MMIYOO_GLES_Flip(void)
{
    GFX_SwapBuffers(g_gles_wait_for_vsync);
}

static SDL_bool
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

    /* SwiftShader's Miyoo framebuffer shim reads two virtual addresses and
     * indexes them with fb_idx % 2. In present-copy mode there is one stable
     * back buffer, so both entries intentionally point at the current draw
     * buffer. In page-flip mode this helper is called again after every swap,
     * refreshing both entries to the newly hidden page before the next frame. */
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

void
MMIYOO_GLES_DefaultProfileConfig(_THIS, int *mask, int *major, int *minor)
{
    if (mask) {
        *mask = SDL_GL_CONTEXT_PROFILE_ES;
    }
    if (major) {
        *major = 2;
    }
    if (minor) {
        *minor = 0;
    }
}

int glLoadLibrary(_THIS, const char *name)
{
    (void)name; /* GLES library is provided by the platform. */
    return 0;
}

void *glGetProcAddress(_THIS, const char *proc)
{
    (void)_this;
    return eglGetProcAddress(proc);
}

void glUnloadLibrary(_THIS)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    if (!gl_data) {
        return;
    }

    if (gl_data->display != EGL_NO_DISPLAY) {
        eglTerminate(gl_data->display);
        gl_data->display = EGL_NO_DISPLAY;
    }
    gl_data->context = EGL_NO_CONTEXT;
    gl_data->surface = EGL_NO_SURFACE;
    gl_data->config = NULL;
    gl_data->buffer_settings_attached = SDL_FALSE;
    gl_data->owns_buffer_settings = SDL_FALSE;
    GFX_SetBackBufferGLESFormat(SDL_FALSE);
}

SDL_GLContext glCreateContext(_THIS, SDL_Window *window)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    MMIYOO_GLESPresentMode_e present_mode = MMIYOO_GLES_ResolvePresentMode();
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLint major = 0;
    EGLint minor = 0;
    EGLint num_configs = 0;
    EGLConfig config = NULL;
    EGLint surface_type_bit = (present_mode == MMIYOO_GLES_PRESENT_PBUFFER) ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT;

    (void)window; /* Miyoo does not expose native window handles. */

    if (!gl_data) {
        SDL_SetError("MMIYOO: missing GL driver data");
        return NULL;
    }

    display = gl_data->display;
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            SDL_SetError("MMIYOO: eglGetDisplay failed (0x%04x)", eglGetError());
            return NULL;
        }

        if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
            SDL_SetError("MMIYOO: eglInitialize failed (0x%04x)", eglGetError());
            return NULL;
        }

        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            SDL_SetError("MMIYOO: eglBindAPI failed (0x%04x)", eglGetError());
            eglTerminate(display);
            return NULL;
        }

        gl_data->display = display;
    }

    config = gl_data->config;
    if (config == NULL) {
        EGLint attribs[32];
        int idx = 0;

        attribs[idx++] = EGL_RED_SIZE;
        attribs[idx++] = (_this->gl_config.red_size > 0) ? _this->gl_config.red_size : 8;
        attribs[idx++] = EGL_GREEN_SIZE;
        attribs[idx++] = (_this->gl_config.green_size > 0) ? _this->gl_config.green_size : 8;
        attribs[idx++] = EGL_BLUE_SIZE;
        attribs[idx++] = (_this->gl_config.blue_size > 0) ? _this->gl_config.blue_size : 8;
        /* windowsurface mode always requests a real alpha channel, ignoring
         * gl_config: the present-copy it uses hardcodes 32bpp for the
         * surface it reads back from, and an alpha_size=0 request lets the
         * vendor EGL pick a packed 24bpp config instead, corrupting every
         * present. pbuffer mode doesn't care either way. */
        attribs[idx++] = EGL_ALPHA_SIZE;
        attribs[idx++] = (present_mode == MMIYOO_GLES_PRESENT_WINDOWSURFACE)
                          ? 8
                          : ((_this->gl_config.alpha_size > 0) ? _this->gl_config.alpha_size : 0);
        attribs[idx++] = EGL_DEPTH_SIZE;
        attribs[idx++] = (_this->gl_config.depth_size > 0) ? _this->gl_config.depth_size : 16;
        attribs[idx++] = EGL_STENCIL_SIZE;
        attribs[idx++] = (_this->gl_config.stencil_size > 0) ? _this->gl_config.stencil_size : 0;
        attribs[idx++] = EGL_SURFACE_TYPE;
        attribs[idx++] = surface_type_bit;
        attribs[idx++] = EGL_RENDERABLE_TYPE;
        attribs[idx++] = EGL_OPENGL_ES2_BIT;
        attribs[idx++] = EGL_COLOR_BUFFER_TYPE;
        attribs[idx++] = EGL_RGB_BUFFER;

        if (_this->gl_config.multisamplebuffers) {
            attribs[idx++] = EGL_SAMPLE_BUFFERS;
            attribs[idx++] = _this->gl_config.multisamplebuffers;
            attribs[idx++] = EGL_SAMPLES;
            attribs[idx++] = SDL_max(_this->gl_config.multisamplesamples, 1);
        }

        attribs[idx++] = EGL_NONE;

        if (eglChooseConfig(display, attribs, &config, 1, &num_configs) != EGL_TRUE) {
            const EGLint alt_attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_SURFACE_TYPE, surface_type_bit,
                EGL_NONE
            };
            EGLint alt_configs = 0;
            if (eglChooseConfig(display, alt_attribs, &config, 1, &alt_configs) != EGL_TRUE || alt_configs <= 0) {
                SDL_SetError("MMIYOO: eglChooseConfig failed (0x%04x)", eglGetError());
                return NULL;
            }
        } else if (num_configs <= 0) {
            SDL_SetError("MMIYOO: no EGL configs match request");
            return NULL;
        }
        gl_data->config = config;
    }

    {
        const EGLint context_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };

        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
        if (context == EGL_NO_CONTEXT) {
            SDL_SetError("MMIYOO: eglCreateContext failed (0x%04x)", eglGetError());
            return NULL;
        }
    }

    if (present_mode == MMIYOO_GLES_PRESENT_PBUFFER) {
        /* A real WindowSurface drives SwiftShader's FrameBufferMMiyoo present
         * path (WindowSurface::swap() -> blit() -> copyRoutine()'s
         * Reactor-JIT blit), which segfaults inside libGLESv2.so on the
         * first-ever eglSwapBuffers() call, on-screen, unconditionally --
         * confirmed via an isolated minimal reproducer independent of
         * RetroArch or this driver's own present code. A PBuffer surface
         * never touches that code path at all: same pattern every
         * offscreen-FBO GL suite in miyoo_sdl2_benchmarks already uses.
         * glSwapWindow() below reads pixels back manually instead of
         * calling eglSwapBuffers(). */
        const EGLint surface_attribs[] = {
            EGL_WIDTH, MMIYOO_GLES_RENDER_WIDTH,
            EGL_HEIGHT, MMIYOO_GLES_RENDER_HEIGHT,
            EGL_NONE
        };

        surface = eglCreatePbufferSurface(display, config, surface_attribs);
        if (surface == EGL_NO_SURFACE) {
            SDL_SetError("MMIYOO: eglCreatePbufferSurface failed (0x%04x)", eglGetError());
            eglDestroyContext(display, context);
            return NULL;
        }
    } else {
        const EGLint surface_attribs[] = {
            EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
            EGL_NONE
        };

        surface = eglCreateWindowSurface(display, config, 0, surface_attribs);
        if (surface == EGL_NO_SURFACE) {
            SDL_SetError("MMIYOO: eglCreateWindowSurface failed (0x%04x)", eglGetError());
            eglDestroyContext(display, context);
            return NULL;
        }
    }

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        SDL_SetError("MMIYOO: eglMakeCurrent failed (0x%04x)", eglGetError());
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        return NULL;
    }

    gl_data->context = context;
    gl_data->surface = surface;
    gl_data->swap_interval = 1;

    if (present_mode == MMIYOO_GLES_PRESENT_PBUFFER) {
        /* No eglUpdateBufferSettings() here: that vendor extension is a
         * WindowSurface-only present hook, and this context uses a PBuffer
         * surface specifically to avoid that present path. See
         * glSwapWindow(). */
    } else if (ppFunc && pfb_idx && pfb_vaddr) {
        if (eglUpdateBufferSettings(display, surface, ppFunc, pfb_idx, pfb_vaddr) == EGL_TRUE) {
            gl_data->buffer_settings_attached = SDL_TRUE;
            gl_data->owns_buffer_settings = SDL_FALSE;
        }
    } else {
        MMIYOO_GLES_UpdateBufferSettings(_this);
    }

    return context;
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

    /* No CPU copy anywhere in this path: glReadPixels() writes directly
     * into gfx.overlay (MI_SYS-allocated, physical address already valid
     * for MI_GFX_BitBlit -- see GFX_GetOverlayVirtual/Physical), and
     * MI_GFX_BitBlit below both hardware-scales AND corrects orientation
     * in one DMA-driven blit. No neon_memcpy, no SDL_memcpy, no per-row
     * loop of any kind. */
    glReadPixels(0, 0, MMIYOO_GLES_RENDER_WIDTH, MMIYOO_GLES_RENDER_HEIGHT,
                 GL_RGBA, GL_UNSIGNED_BYTE, overlay_virt);

    srcrect.x = 0;
    srcrect.y = 0;
    srcrect.w = MMIYOO_GLES_RENDER_WIDTH;
    srcrect.h = MMIYOO_GLES_RENDER_HEIGHT;

    /* Hardware-scaled present: source is the small render, destination is
     * the full real panel -- MI_GFX_BitBlit does the upscale. 640/320 and
     * 480/240 are both exact 2x, so this is a clean integer scale. */
    dstrect.x = 0;
    dstrect.y = 0;
    dstrect.w = (int)GFX_GetFrameWidth();
    dstrect.h = (int)GFX_GetFrameHeight();

    /* glReadPixels(GL_RGBA) byte order R,G,B,A in memory is
     * E_MI_GFX_FMT_ABGR8888 by MI_GFX's naming (first-named channel =
     * highest byte) -- same mapping SDL_PIXELFORMAT_ABGR8888 uses.
     *
     * MIRROR_HORIZONTAL, not a CPU row-flip + ROTATE_180: composing the
     * panel's required ROTATE_180 (see My_QueueCopy's
     * `base_rotation = is_target_texture ? ROTATE_0 : ROTATE_180`) with
     * glReadPixels' inherent bottom-up row order algebraically cancels the
     * Y component, leaving a pure X-flip. Doing that flip in MI_GFX
     * instead of on the CPU removes the last CPU-side pixel touch from
     * this path entirely -- MI_GFX_BitBlit (DMA-driven hardware blit) does
     * 100% of the pixel movement now. */
    if (GFX_Copy(overlay_virt, overlay_phy, srcrect, dstrect, pitch,
                 E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_HORIZONTAL, SDL_BLENDMODE_NONE,
                 NULL, NULL, SDL_FALSE,
                 0, E_MI_GFX_FMT_ABGR8888, 4,
                 255, 255, 255, 255) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "MMIYOO GLES: GFX_Copy present blit failed");
    }

    /* MI_GFX_BitBlit is asynchronous (fire-and-forget with a fence) and
     * gfx.overlay is a single shared buffer, not double-buffered -- without
     * waiting here, next frame's glReadPixels() can start overwriting it
     * while this frame's hardware blit is still reading from it, producing
     * exactly the kind of torn/flickering frame this caused before this
     * flush was added. */
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

int glMakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    EGLContext egl_context = (EGLContext)context;
    EGLSurface draw_surface;

    (void)window;

    if (!gl_data || gl_data->display == EGL_NO_DISPLAY) {
        return SDL_SetError("MMIYOO: make current without display");
    }

    draw_surface = (egl_context == EGL_NO_CONTEXT) ? EGL_NO_SURFACE : gl_data->surface;

    if (eglMakeCurrent(gl_data->display, draw_surface, draw_surface, egl_context) != EGL_TRUE) {
        return SDL_SetError("MMIYOO: eglMakeCurrent failed (0x%04x)", eglGetError());
    }

    gl_data->context = egl_context;
    return 0;
}

void glDeleteContext(_THIS, SDL_GLContext context)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    EGLContext egl_context = (EGLContext)context;

    if (!gl_data || gl_data->display == EGL_NO_DISPLAY) {
        return;
    }

    if (gl_data->context == egl_context) {
        eglMakeCurrent(gl_data->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        gl_data->context = EGL_NO_CONTEXT;
    }

    if (gl_data->surface != EGL_NO_SURFACE) {
        eglDestroySurface(gl_data->display, gl_data->surface);
        gl_data->surface = EGL_NO_SURFACE;
    }

    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(gl_data->display, egl_context);
    }
}

#endif /* SDL_VIDEO_DRIVER_MMIYOO && SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2 */
