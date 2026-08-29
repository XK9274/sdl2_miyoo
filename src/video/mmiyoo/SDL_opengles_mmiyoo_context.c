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

#if SDL_VIDEO_DRIVER_MMIYOO && SDL_VIDEO_OPENGL_EGL && SDL_VIDEO_OPENGL_ES2

#include "SDL_video_mmiyoo.h"
#include "SDL_opengles_mmiyoo.h"
#include "SDL_opengles_mmiyoo_internal.h"
#include <GLES2/gl2.h>

/* Library loading, EGL config selection, context creation/deletion,
 * present-surface selection (pbuffer vs windowsurface) needed by context
 * creation, make-current behavior, and profile defaults. */

static MMIYOO_GLESPresentMode_e g_present_mode = MMIYOO_GLES_PRESENT_PBUFFER;
static SDL_bool g_present_mode_resolved = SDL_FALSE;

MMIYOO_GLESPresentMode_e
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
         * first-ever eglSwapBuffers() call, unconditionally. A PBuffer
         * surface never touches that code path at all; glSwapWindow()
         * below reads pixels back manually instead of calling
         * eglSwapBuffers(). */
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
