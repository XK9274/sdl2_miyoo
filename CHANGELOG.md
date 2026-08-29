# SDL2 Miyoo Mini — new_miyoo Release Notes

This branch represents a substantial rework of the SDL2 Miyoo Mini port, migrated from the older `mmiyoo` toolchain to the `union-miyoomini-toolchain` ecosystem and overhauled across audio, render, video, and EGL subsystems. The changes below are things that mattered — failures that were fixed, bottlenecks that were removed, and dead weight that was cleaned out.

---

## Migration Notes

If you were building against a previous version of this port, these are the things you'll need to address:

**Toolchain path has changed.** The build now targets `/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc` (shauninman's `union-miyoomini-toolchain`). The old `mmiyoo` toolchain at `/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot` is no longer referenced. The host triple is `arm-linux-gnueabihf` — not `arm-linux`.

**`json-c`, `SDL_image`, `SDL_ttf`, and `shmvar` are no longer linked.** If your application was relying on these being transitively available through this library, you'll need to link them directly. They don't belong in an SDL backend and they aren't present in the union toolchain sysroot anyway.

**`SDL_Unsupported()` diagnostic mode is opt-in.** Set `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS` to `"1"` at runtime to get call-site logging for every unimplemented backend path. Off by default, no behaviour change if you don't set it.

---

## Audio Backend

**The hardcoded sample rate is gone.** The old driver cast the application's requested frequency directly to `MI_AUDIO_SampleRate_e` and assumed 768 frames per period regardless of what was asked for. When an emulator or game requested 22050 Hz, 11025 Hz, or any rate that didn't align with that assumption, the MI AO API either failed silently or produced garbled output. The new `MMIYOO_SelectSampleRate()` finds the closest rate the MI hardware actually supports (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 Hz) and normalises the application's requested frequency in place, so the rest of SDL's audio pipeline uses the corrected value.

The teardown order is now: flush the channel buffer, disable the channel, disable the device, clear the public attributes — gated on an `ao_active` flag so it's a no-op on a device that was never successfully opened. The teardown crash/hang seen when closing audio mid-playback is fixed.

The old driver also had an adaptive delay algorithm in `WaitDevice` — a `buffer_diff` / `adjustment_factor` / `scaling_factor` feedback loop that tried to tune sleep duration dynamically. It was removed. The new path checks channel status and retries with a 1ms sleep on `MI_AO_ERR_NOBUF`. Simpler, less wrong.

`mixlen` is now `Uint32` (was `int`). `SDL_PrivateAudioData` gains `frame_bytes`, a persistent `MI_AUDIO_Frame_t frame` (initialised once in `OpenDevice`, reused per-call), and `SDL_bool ao_active`.

---

## Render Backend

**Fence batching.** The old renderer called `MI_GFX_WaitAllDone()` or waited on each fence immediately after submitting each blit operation. On a sequence of draws, this meant the CPU stalled after every single call waiting for the GFX hardware to confirm completion before submitting the next one. The new renderer accumulates up to 512 fence IDs in `pending_fences[]` and flushes them in a batch via `flush_fence_batch()`. Multiple GFX operations are queued to hardware and waited on together, which eliminates the per-blit stall.

**Per-texture MI memory.** `MMIYOO_TextureData` now tracks `phyAddr` (physical address), `virAddr` (virtual mapping), `uses_msys_memory`, and a `MI_GFX_Surface_t gfx_surface`. Textures allocated in physically-contiguous MSYS memory can be handed to the MI GFX engine as DMA source buffers directly, without a CPU copy on every render. Previously all renders went through a copy path. The `mi_format` and `bytes_per_pixel` fields are cached on texture creation to avoid recalculation per blit.

**NEON acceleration.** `libneonarmmiyoo` is now linked. NEON paths (guarded by `__ARM_NEON`) are used for pixel copy and scale operations in cases where the MI GFX engine can't be used — software fallback paths that previously used scalar memory copies.

**`SDL_RenderCopyEx` with non-zero rotation logs once, not every frame.** The MI GFX engine doesn't support arbitrary rotation. The old driver had no guard on this — if a game called `SDL_RenderCopyEx` with a rotation angle every frame, the log filled with repeated "not supported" messages at frame rate. `g_warned_copyex_angle` suppresses subsequent warnings after the first.

**Texture leak detection.** `mmiyoo_texture_live_count` is incremented on texture create and decremented on destroy. Error messages from texture operations include the current live count, which makes it straightforward to spot leaks from a log file.

**Verbose debug logging** is controlled by `mmiyoo_debug_verbose` (off by default) via the `MMIYOO_VERBOSE_LOG` macro. Enables per-blit trace output without recompiling.

---

## Video Backend and GFX Structure

**Framebuffer geometry is now dynamic.** The old backend hardcoded `FB_W 640`, `FB_H 480`, `FB_BPP 4`, and derived constants. Any display configuration that differed broke silently. These constants are gone. `MMIYOO_UpdateFramebufferMetrics()` queries the actual framebuffer geometry on init and caches it in `g_framebuffer_width/height/stride/bytes_per_pixel/size`. `MMIYOO_FrameBytesPerPixel()` and `MMIYOO_FrameStrideBytes()` compute from these at call time.

**Double-buffering infrastructure.** The `GFX` struct gains a `back` DMA buffer alongside the existing `fb`/`tmp`/`overlay` entries, with a `length` field on all DMA buffer entries. `double_buffer_enabled`, `fb_dev`, and an async video thread (`video_thread`, `action_mutex`, `action_cond`) are added for frame presentation. The threading infrastructure enables decoupled frame submission.

**Crash diagnostics.** `crash_handler()` is registered for `SIGSEGV`/`SIGABRT` and prints a backtrace via `execinfo.h`. On a device with no attached debugger and no core dumps, a backtrace to a log file is the difference between diagnosable and not.

The application-layer code that was embedded in the video backend — JSON config file parsing via `json-c`, image loading via SDL_image, TTF text rendering — has been removed entirely.

---

## OpenGL ES / EGL

**EGL state was global.** `display`, `context`, `surface`, and `config` were bare file-scope statics. When an application created and destroyed an SDL GL context multiple times — which emulators do routinely — the globals went stale on the second create/destroy cycle. EGL handles are now members of `SDL_GLDriverData` (the per-context driver struct), with `config` and `swap_interval` added as new fields. Each context owns its EGL state.

**`eglUpdateBufferSettings` was a hardcoded extern.** This is a Mstar proprietary extension not in any standard EGL header. The old code declared it as an extern function with a fixed signature, creating an implicit link-time dependency on an undocumented symbol. It's now resolved at runtime via `eglGetProcAddress` and cached as a typed `PFNEGLUPDATEBUFFERSETTINGSPROC` function pointer. If the extension isn't present, the pointer is NULL and the call is skipped. No link dependency.

**`glUnloadLibrary` could crash on a NULL `gl_data`.** The old teardown called `eglTerminate(_this->gl_data->display)` without checking whether `gl_data` was non-NULL. The new version guards against NULL `gl_data` and resets all handles to `EGL_NO_DISPLAY`/`EGL_NO_CONTEXT`/`EGL_NO_SURFACE` sentinels after teardown, so double-free is safe.

**`glGetSwapInterval` was missing.** SDL's GL driver interface requires it. Its absence meant a NULL function pointer in the driver vtable — benign if never called, but a latent crash if something did call it. It's now implemented.

`MMIYOO_GLES_DefaultProfileConfig()` sets ES2 as the default GL profile. `glCreateContext` was expanded to include proper EGL config selection, multisampling support, and error path cleanup.

---

## Diagnostics

**`SDL_Unsupported()` now captures the call site.** When porting applications to the Miyoo backend, operations that hit unimplemented paths returned -1 with `SDL_UNSUPPORTED` and no other information — no function name, no file, no line. Tracing which SDL call triggered the error required adding custom logging to the application. The macro now expands to `SDL_Unsupported_REAL(__FILE__, __LINE__, SDL_FUNCTION)`. With `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS` set to `"1"`, the backing function logs the call site at warn level to `SDL_LOG_CATEGORY_ERROR`. Set the hint, run your app, see exactly which functions are hitting unimplemented paths.

**`CHECK_RENDERER_MAGIC`, `CHECK_TEXTURE_MAGIC`, `CHECK_WINDOW_MAGIC` log diagnostics before failing.** These macros previously called `SDL_SetError` and returned. On a device logging to a serial console, an `Invalid renderer` error with no additional context is useless — it doesn't tell you whether the pointer was NULL, freed, from a different context, or a wrong cast. The new versions log the pointer value, the magic value at that address, and the expected magic value. Use-after-free (magic is garbage) is distinguishable from wrong-context (magic is valid but wrong) at a glance.

**`SDL_RenderCopyF`/`SDL_RenderCopyExF` with NULL texture return 0 instead of asserting.** Some games check for NULL textures at their own layer and conditionally skip the draw call, but don't account for SDL's assert path in the same code. The NULL check now returns 0 and logs a debug message rather than hitting `SDL_assert`.

---

## Dependency Cleanup and Dead Code

**Removed libraries:** `json-c`, `SDL_image`, `SDL_ttf` (video backend was doing application-layer work), `shmvar` (not in union toolchain sysroot), `mi_common` (subsumed by `mi_sys` in the union toolchain).

**Added libraries:** `mi_sys`, `mi_gfx`, `mi_ao` (MI hardware access), `neonarmmiyoo` (NEON helpers), `EGL`, `GLESv2`, `rt` (for MI timing interfaces).

**`nds_touch/` removed.** Nintendo DS dual-screen touch input code, carried over from a different device variant. The Miyoo Mini doesn't have an NDS-style dual-screen interface.

**`hex_pen.h` and stylus input removed.** Pen digitiser input for a platform that has no stylus. Dead from the start on Miyoo Mini.

**`BASE_REG_RIU_PA`, `BASE_REG_MPLL_PA` removed.** Hardware register base addresses that weren't used in any active code path.

**MI headers centralised to `include/`.** Previously each subsystem (`src/audio/mmiyoo/`, `src/video/mmiyoo/`) kept its own copies of the MI headers, which had diverged through local modifications. All MI headers are now in `include/` and wrapped in `#ifdef MMIYOO` in `SDL_video_mmiyoo.h`.

**Sensor backend re-enabled.** `SDL_SENSOR_DUMMY 1` is set in `SDL_config.h`; `SDL_SENSOR_DISABLED` is no longer forced. The dummy backend means sensor capability queries succeed rather than hitting a compile-time disabled path.

---

## New Backend Implementations

Functions added to the mmiyoo backend that were not present in vanilla.

### Audio (`SDL_audio_mmiyoo.c`)

| Function | Description |
|---|---|
| `MMIYOO_SelectSampleRate()` | Maps requested frequency to the closest MI-supported rate (8000–48000 Hz) and normalises the spec in place |

### Video (`SDL_video_mmiyoo.c`)

| Function | Description |
|---|---|
| `MMIYOO_FrameBytesPerPixel()` | Returns BPP from queried framebuffer geometry |
| `MMIYOO_FrameStrideBytes()` | Returns stride from queried framebuffer geometry |
| `MMIYOO_UpdateFramebufferMetrics()` | Queries actual fb dimensions on init and populates globals |
| `GFX_FlushTextureFences()` | Drains the pending MI GFX fence queue |
| `GFX_GetFrameBuffer()` | Returns physical address of the active framebuffer |
| `GFX_GetFrameStride()` | Returns framebuffer stride in bytes |
| `GFX_GetFrameWidth()` / `GFX_GetFrameHeight()` | Framebuffer dimension getters |
| `GFX_GetFrameBufferVirtual()` | Returns virtual address mapping of the framebuffer |
| `GFX_IsDoubleBuffered()` | Returns whether double-buffering is active |
| `GFX_SwapBuffers()` | Presents the back buffer (replaces the old `GFX_Flip()`) |
| `fb_init()` / `fb_uninit()` | Reference-counted framebuffer init/teardown with back buffer DMA allocation |
| `video_handler()` | Async video thread for decoupled frame presentation |
| `crash_handler()` | SIGSEGV/SIGABRT signal handler that prints a backtrace via `execinfo.h` |
| `MMIYOO_DestroyWindow()` | Window teardown — not present in vanilla |

### OpenGL ES (`SDL_opengles_mmiyoo.c`)

| Function | Description |
|---|---|
| `MMIYOO_GLES_DefaultProfileConfig()` | Sets ES2 profile defaults for the GL driver |
| `glGetSwapInterval()` | Returns current swap interval — absent from vanilla, leaving a null slot in the driver vtable |

`glCreateContext()` was substantially rewritten (vanilla ~80 lines → ~140): adds proper EGL config selection, multisampling support, and full error path handling.

### Render (`SDL_render_mmiyoo.c`)

Vanilla had only six stub-level functions. The new backend adds a full render pipeline:

**Fence batching**

| Function | Description |
|---|---|
| `flush_batch()` | Waits on all accumulated MI GFX fences in one pass |
| `add_fence_to_batch()` | Queues a fence ID; flushes when batch is full |

**Primitive drawing**

| Function | Description |
|---|---|
| `MMIYOO_ExecuteQuickFill()` | Fast MI GFX fill for solid rectangles |
| `MMIYOO_ExecuteDrawLine()` | Hardware-accelerated or software line draw |
| `MMIYOO_DrawFilledTriangle()` | Scanline rasterizer for filled triangles |
| `MMIYOO_ComputeClipCode()` / `MMIYOO_ClipLineToRect()` | Cohen-Sutherland line clipping |

**Batched primitive accumulation**

| Function | Description |
|---|---|
| `MMIYOO_LineBatchReset/Accumulate/Flush()` | Accumulates line segments and flushes as a single GFX call |
| `MMIYOO_RectBatchFlush/Accumulate()` | Same for filled rectangles |
| `MMIYOO_PrepareDrawRect()` | Selects fill or outline path and submits to the appropriate batch |

**Render command dispatch**

| Function | Description |
|---|---|
| `MMIYOO_ProcessFillCommand()` | Handles `SDL_RENDERCMD_FILL_RECTS` |
| `MMIYOO_ProcessDrawLines()` | Handles `SDL_RENDERCMD_DRAW_LINES` |
| `MMIYOO_ProcessGeometry()` | Handles `SDL_RENDERCMD_GEOMETRY` via the triangle rasterizer |

**State and coordinate management**

| Function | Description |
|---|---|
| `MMIYOO_SetRenderTarget()` | Switches render target between framebuffer and texture surfaces |
| `MMIYOO_UpdateClipState()` | Applies or clears the MI GFX clip rectangle |
| `MMIYOO_GetTargetBounds()` | Returns the bounds rect of the current render target |
| `MMIYOO_TransformToTarget()` / `MMIYOO_ApplyViewportToPoint()` | Viewport and target coordinate transforms |
| `MMIYOO_GetFramebufferWidth/Height()` / `MMIYOO_GetCurrentTargetWidth/Height()` | Dimension getters for the active render target |

**Copy and transform**

| Function | Description |
|---|---|
| `MMIYOO_ExecuteCopyCommand()` | Full-featured blit: rotation, flip, clip rect, blend mode, MI GFX or NEON fallback |
| `MMIYOO_AddRotations()` / `MMIYOO_RotationSwapsAxes()` | MI GFX rotation composition helpers |
| `MMIYOO_FlipToMirror()` | Maps `SDL_RendererFlip` to `MI_GFX_Mirror_e` |

---

## Build and CI

**GitHub Actions** are now set up for this repo:

- `build.yml` — triggers on push and pull request to any branch. Installs shauninman's toolchain (v0.0.3), builds `libneonarmmiyoo` with the cross tools, configures and builds SDL2, strips the output, and uploads `libSDL2-2.0.so.0` as a workflow artifact.
- `release.yml` — triggers on `vMAJOR.MINOR.PATCH` tags. Runs preflight checks (semver format, clean working tree, no existing release for the tag) before building and publishing a GitHub release with the `.so` attached.

**`build-scripts/mk_miyoo.sh`** is a standalone build script that works from anywhere inside or outside the source tree. It requires `/opt/miyoomini-toolchain` to be present, or accepts `--docker` to clone and build the toolchain image automatically. See `README.md` for the full option list.
