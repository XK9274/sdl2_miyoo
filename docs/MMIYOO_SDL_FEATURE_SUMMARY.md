# Miyoo SDL Feature Summary

## Hints

- `SDL_MMIYOO_INPUT_MODE` -- `"keyboard"` / `"joystick"` (unset = joystick), live-switchable
- `SDL_MMIYOO_VSYNC_MODE` -- `"off"` (default, temporary decision - see comment on `SDL_HINT_MMIYOO_VSYNC_MODE` in `SDL_mmiyoo.h`) / `"adaptive"` / `"strict"` (real `/dev/l`-paced panning, launch-time only). Wins outright if set; if unset, the standard `SDL_RENDERER_PRESENTVSYNC`/`SDL_RenderSetVSync()` request decides adaptive-vs-off instead. Verified (not just requested) state is queryable via `SDL_GetRendererInfo()` -- `SDL_RENDERER_PRESENTVSYNC` is only set when presentation is genuinely vsync-paced.
- `SDL_MMIYOO_DEBUG` / `SDL_MMIYOO_DEBUG_VERBOSE` -- raise driver log verbosity
- `SDL_MMIYOO_GEOMETRY_STATS` -- log per-present triangle/span stats
- `SDL_MMIYOO_GEOMETRY_BAND_HEIGHT` -- triangle rasterizer span band height (1-32, default 3)
- `SDL_MMIYOO_GEOMETRY_QUICKPATH` -- opt into skipping the duplicate blit from a glyph quad's second identical textured triangle
- `SDL_MMIYOO_FRAME_TIMING` -- log per-second frame timing, including command queue, present, blit count, and command-category breakdown
- `SDL_MMIYOO_TEXTURE_POOL` -- enable/disable the bounded MI_SYS texture-memory reuse pool (default enabled)
- `SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES` -- cap the texture-memory reuse pool byte budget (default 10 MiB)
- `SDL_MMIYOO_INTEGER_SCALE` -- enable the default software integer upscaler for eligible core-content blits; set to `0` to disable
- `SDL_MMIYOO_DEBUG_LOG` -- enable additional scaling and input diagnostics
- `SDL_VIDEO_MMIYOO_SAVE_FRAMES` -- dump each presented frame to a BMP

## Miyoo-specific functions

- Sysfs write helper: `MMIYOO_WriteSysfs`
- Integer file read helper: `MMIYOO_ReadIntFile`
- Device model detection (parses `fw_printenv SdUpgradeImage`, works under any CFW): `MMIYOO_GetDeviceModel`
- CFW (custom firmware) brand detection (stub, returns `MMIYOO_CFW_UNKNOWN` pending marker specifics): `MMIYOO_GetCFW`
- Keycode to button-bitmask mapping: `MMIYOO_KeycodeToButtonMask`
- Default framebuffer info: `MMIYOO_GetDefaultFramebufferInfo`
- Framebuffer info from an open fd: `MMIYOO_GetFramebufferInfoFromFD`
- Framebuffer metrics refresh: `MMIYOO_UpdateFramebufferMetrics`
- Rumble capability probe: `MMIYOO_HasRumble`
- Rumble GPIO control: `MMIYOO_SetRumble`
- Rumble GPIO init: `MMIYOO_InitRumbleGPIO`
- Battery percent probe: `MMIYOO_GetBatteryPercent`
- Charging-state probe: `MMIYOO_IsCharging`
- Battery percent from raw ADC: `MMIYOO_BatteryPercentFromADC`
- Mini battery ADC read: `MMIYOO_ReadMiniBatteryADC`
- Charging state via GPIO: `MMIYOO_IsChargingGPIO`
- Charging/battery via axp_test: `MMIYOO_ReadAxpTest`
- Shared /dev/input/event0 reader init/deinit: `MMIYOO_InputInit`, `MMIYOO_InputDeinit`
- Shared input reader thread: `MMIYOO_InputThread`
- Keypad bitmap snapshot: `MMIYOO_GetKeypadBitmap`
- Keyboard/joystick mode getters: `MMIYOO_IsKeyboardModeActive`, `MMIYOO_IsJoystickModeActive`
- VSync mode getters: `MMIYOO_GetVSyncMode`, `MMIYOO_ResolvePresentVSyncMode`
- Window focus restore: `MMIYOO_RaiseWindow`
- Video driver availability check (explicit `SDL_VIDEODRIVER=mmiyoo` override, then falls back to `MMIYOO_ProbeHardware`): `MMIYOO_Available`
- Real hardware probe for auto-detection (checks `/dev/mi_sys` and `/customer/app/MainUI` exist): `MMIYOO_ProbeHardware`
- Framebuffer clear: `FB_Clear`
- Framebuffer init/teardown: `FB_Init`, `FB_Uninit`
- Present (copy or real page-flip depending on vsync mode): `GFX_SwapBuffers`
- GFX blit wrapper (blend/colormod/clip): `GFX_Copy`
- Texture fence batching: `GFX_FlushTextureFences`, `GFX_AddTextureFence`
- Framebuffer/page-flip accessors: `GFX_GetFrameBuffer`, `GFX_GetFrameBufferVirtual`, `GFX_GetFrameStride`, `GFX_GetFrameWidth`, `GFX_GetFrameHeight`, `GFX_IsPageFlipEnabled`
- GLES default profile config: `MMIYOO_GLES_DefaultProfileConfig`
- GLES buffer-settings extension hook: `glUpdateBufferSettings`
- GLES overlay buffer accessors (pbuffer present mode): `GFX_GetOverlayVirtual`, `GFX_GetOverlayPhysical`
- Renderer copy-command executor: `MMIYOO_ExecuteCopyCommand` (internal, `SDL_render_mmiyoo_commands.c`)
- Sample-rate selection: `MMIYOO_SelectSampleRate`
- Gamepad mapping: `MMIYOO_JoystickGetGamepadMapping`
- Rumble auto-stop timer: `MMIYOO_HapticTimer`

## Source layout

`src/render/mmiyoo/`:
- `SDL_render_mmiyoo.c` -- renderer driver descriptor, vtable wiring, renderer creation/destruction, top-level lifecycle
- `SDL_render_mmiyoo_internal.h` -- private cross-file contract for the files below (not public API)
- `SDL_render_mmiyoo_geometry.c` -- viewport/clip/triangle-rasterization primitives, QuickFill/DrawLine execution
- `SDL_render_mmiyoo_texture.c` -- pixel-format conversion, MMA allocation, the bounded texture pool, texture lifecycle
- `SDL_render_mmiyoo_commands.c` -- SDL command-queue producers/execution, line/rect batching, the copy-command executor
- `SDL_render_mmiyoo_scaling.c` -- integer upscaling, downscale-composite, stretch-fill, NEON scaler selection
- `SDL_render_mmiyoo_present.c` -- render-target readback, present-vsync selection, renderer presentation

`src/video/mmiyoo/`:
- `SDL_video_mmiyoo.c` -- device/window creation, display-mode setup, video init/quit
- `SDL_video_mmiyoo_internal.h` -- private GFX/window state and GFX/FB internals
- `SDL_video_mmiyoo_gfx.c` -- framebuffer metrics, system/GFX init+teardown, the shared MI_GFX blit implementation
- `SDL_video_mmiyoo_present.c` -- buffer swap/page-flip, framebuffer/overlay accessors, presentation pacing
- `SDL_opengles_mmiyoo.h` -- compatibility-facing GLES declarations (unchanged)
- `SDL_opengles_mmiyoo_internal.h` -- private present-mode/buffer-settings contract between the two files below
- `SDL_opengles_mmiyoo_context.c` -- library loading, EGL config selection, context creation/deletion
- `SDL_opengles_mmiyoo_swap.c` -- swap interval, pbuffer/windowsurface presentation, buffer-settings handling

## SDL driver functionality supported

**Video** (`SDL_VideoDevice`): `CreateDevice`, `DeleteDevice`, `VideoInit`, `VideoQuit`, `SetDisplayMode` (no-op), `CreateWindow`, `CreateWindowFrom`, `DestroyWindow`, `PumpEvents`, `CreateWindowFramebuffer`, `UpdateWindowFramebuffer`, `DestroyWindowFramebuffer`

**Render** (`SDL_Renderer`, driver name `MMIYOO`): `CreateRenderer`, `WindowEvent`, `CreateTexture`, `UpdateTexture`, `LockTexture`, `UnlockTexture`, `SetTextureScaleMode` (no-op), `SetRenderTarget`, `QueueSetViewport`, `QueueSetDrawColor`, `QueueDrawPoints`, `QueueDrawLines`, `QueueGeometry`, `QueueFillRects`, `QueueCopy`, `QueueCopyEx`, `RunCommandQueue`, `RenderPresent`, `DestroyTexture`, `DestroyRenderer`, `SetVSync`

- Supported render paths: hardware `QuickFill` for clears/fills/axis-aligned line batches and eligible opaque 1x1-texture geometry fills; `MI_GFX_BitBlit` for texture copy/copy-ex with blend, color modulation, clipping, orthogonal rotation, and flip; software triangle rasterization for untextured geometry; textured geometry as one bounded hardware blit per triangle, with optional glyph-quad duplicate blit skip via `SDL_MMIYOO_GEOMETRY_QUICKPATH`; eligible integer upscaling, stretch-fill, and oversized render-target downscale-composite paths.
- Texture allocation: `CreateTexture` uses a bounded MI_SYS MMA reuse pool to reduce allocation churn; the pool can be disabled or capped with the texture-pool hints.
- Readback: `RenderReadPixels` supports render-target textures with CPU-mapped storage; the default framebuffer target remains unsupported because its runtime format is not verified.
- Pixel formats: `SDL_PIXELFORMAT_RGBA8888` has no matching native MI_GFX format on this little-endian target (its real byte order is A,B,G,R) and is remapped to `ARGB8888`, which silently swaps R/B for that format's pixel data -- use `SDL_PIXELFORMAT_ARGB8888`, `ABGR8888`, or `BGRA8888` instead.

**Audio** (`SDL_AudioDriverImpl`, driver name `MMIYOO`): `OpenDevice`, `WaitDevice`, `PlayDevice`, `GetDeviceBuf`, `CloseDevice`

- Not supported: capture (`OnlyHasDefaultOutputDevice`)

**Joystick** (`SDL_JoystickDriver`): `Init`, `GetCount`, `Detect`, `GetDeviceName`, `GetDeviceGUID`, `GetDeviceInstanceID`, `Open`, `Update`, `Close`, `Quit`, `Rumble`, `GetCapabilities`, `GetGamepadMapping`

- Not supported: `GetDevicePlayerIndex`/`SetDevicePlayerIndex` (stub), `RumbleTriggers`, `SetLED`, `SendEffect`, `SetSensorsEnabled`

**Haptic** (`SDL_SYS_Haptic*`): `Init`, `NumHaptics`, `HapticName`, `HapticOpen`, `HapticOpenFromJoystick`, `JoystickIsHaptic`, `JoystickSameHaptic`, `HapticClose`, `HapticQuit`, `HapticNewEffect`, `HapticUpdateEffect`, `HapticDestroyEffect`, `HapticRunEffect`, `HapticStopEffect`, `HapticStopAll`, `HapticGetEffectStatus`, `HapticPause`, `HapticUnpause`

- Effect type supported: `SDL_HAPTIC_LEFTRIGHT` only
- Not supported: `HapticMouse`, `SetAutocenter`

**Power** (`SDL_GetPowerInfo`): supported, via `SDL_GetPowerInfo_MMIYOO` -- no time-remaining estimate (`seconds` always -1)

**GLES** (`SDL_GLDriverData`, built only with `--enable-gles`): `glLoadLibrary`, `glGetProcAddress`, `glUnloadLibrary`, `glCreateContext`, `glMakeCurrent`, `glDeleteContext`, `glSwapWindow`, `glSetSwapInterval`, `glGetSwapInterval`

Two present strategies, selected via `SDL_MMIYOO_GLES_PRESENT_MODE`:

- `pbuffer` (default): an EGL PBuffer surface, `glReadPixels` into `gfx.overlay`, then a single hardware `GFX_Copy`/`MI_GFX_BitBlit` (scale + orientation) to present. Colour-correct; still bound by SwiftShader's software-rasterizer performance ceiling.
- `windowsurface`: a real EGL WindowSurface presented through the vendor `eglUpdateBufferSettings` extension into `gfx.back` via `GFX_SwapBuffers`. Kept for comparison/regression testing only -- known colour corruption (green patches) in translucent UI regions, root cause not found. See TODO.
