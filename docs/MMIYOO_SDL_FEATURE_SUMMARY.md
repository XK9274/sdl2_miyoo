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
- `SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES` -- cap the texture-memory reuse pool byte budget (default 2 MiB)
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
- Mouse bounds setter: `MMIYOO_SetMouseBounds`
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
- Renderer copy helper: `My_QueueCopy`
- Sample-rate selection: `MMIYOO_SelectSampleRate`
- Gamepad mapping: `MMIYOO_JoystickGetGamepadMapping`
- Rumble auto-stop timer: `MMIYOO_HapticTimer`

## SDL driver functionality supported

**Video** (`SDL_VideoDevice`): `CreateDevice`, `DeleteDevice`, `VideoInit`, `VideoQuit`, `SetDisplayMode` (no-op), `CreateWindow`, `CreateWindowFrom`, `DestroyWindow`, `PumpEvents`, `CreateWindowFramebuffer`, `UpdateWindowFramebuffer`, `DestroyWindowFramebuffer`

**Render** (`SDL_Renderer`, driver name `MMIYOO`): `CreateRenderer`, `WindowEvent`, `CreateTexture`, `UpdateTexture`, `LockTexture`, `UnlockTexture`, `SetTextureScaleMode` (no-op), `SetRenderTarget`, `QueueSetViewport`, `QueueSetDrawColor`, `QueueDrawPoints`, `QueueDrawLines`, `QueueGeometry`, `QueueFillRects`, `QueueCopy`, `QueueCopyEx`, `RunCommandQueue`, `RenderPresent`, `DestroyTexture`, `DestroyRenderer`, `SetVSync`

- Supported render paths: hardware `QuickFill` for clears/fills/axis-aligned line batches and eligible opaque 1x1-texture geometry fills; `MI_GFX_BitBlit` for texture copy/copy-ex with blend, color modulation, clipping, orthogonal rotation, and flip; software triangle rasterization for untextured geometry; textured geometry as one bounded hardware blit per triangle, with optional glyph-quad duplicate blit skip via `SDL_MMIYOO_GEOMETRY_QUICKPATH`.
- Texture allocation: `CreateTexture` uses a bounded MI_SYS MMA reuse pool to reduce allocation churn; the pool can be disabled or capped with the texture-pool hints.
- Not supported: `RenderReadPixels`

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
