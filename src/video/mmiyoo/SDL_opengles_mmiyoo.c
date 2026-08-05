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

typedef EGLBoolean (EGLAPIENTRY *PFNEGLUPDATEBUFFERSETTINGSPROC)(EGLDisplay, EGLSurface, void *, void *, void *);

static PFNEGLUPDATEBUFFERSETTINGSPROC p_eglUpdateBufferSettings = NULL;
static void *ppFunc = NULL;
static void *pfb_idx = NULL;
static void *pfb_vaddr = NULL;

// EGLBoolean eglUpdateBufferSettings(EGLDisplay display, EGLSurface surface, void *pFunc, void *fb_idx, void *fb_vaddr);

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

    /* Cache optional extension entry points we care about. */
    if (!p_eglUpdateBufferSettings) {
        p_eglUpdateBufferSettings = (PFNEGLUPDATEBUFFERSETTINGSPROC)eglGetProcAddress("eglUpdateBufferSettings");
    }

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
}

SDL_GLContext glCreateContext(_THIS, SDL_Window *window)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLint major = 0;
    EGLint minor = 0;
    EGLint num_configs = 0;
    EGLConfig config = NULL;

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

        if (!p_eglUpdateBufferSettings) {
            p_eglUpdateBufferSettings = (PFNEGLUPDATEBUFFERSETTINGSPROC)eglGetProcAddress("eglUpdateBufferSettings");
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
    attribs[idx++] = EGL_ALPHA_SIZE;
    attribs[idx++] = (_this->gl_config.alpha_size > 0) ? _this->gl_config.alpha_size : 0;
    attribs[idx++] = EGL_DEPTH_SIZE;
    attribs[idx++] = (_this->gl_config.depth_size > 0) ? _this->gl_config.depth_size : 16;
    attribs[idx++] = EGL_STENCIL_SIZE;
    attribs[idx++] = (_this->gl_config.stencil_size > 0) ? _this->gl_config.stencil_size : 0;
    attribs[idx++] = EGL_SURFACE_TYPE;
    attribs[idx++] = EGL_WINDOW_BIT;
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
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
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

    {
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

    if (p_eglUpdateBufferSettings && ppFunc && pfb_idx && pfb_vaddr) {
        p_eglUpdateBufferSettings(display, surface, ppFunc, pfb_idx, pfb_vaddr);
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

int glSwapWindow(_THIS, SDL_Window *window)
{
    SDL_GLDriverData *gl_data = (SDL_GLDriverData *)_this->gl_data;
    (void)window;

    if (!gl_data || gl_data->display == EGL_NO_DISPLAY || gl_data->surface == EGL_NO_SURFACE) {
        return SDL_SetError("MMIYOO: no EGL surface to swap");
    }

    if (eglSwapBuffers(gl_data->display, gl_data->surface) != EGL_TRUE) {
        return SDL_SetError("MMIYOO: eglSwapBuffers failed (0x%04x)", eglGetError());
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
