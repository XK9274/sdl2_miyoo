# Miyoo SDL Feature Summary

## Hints

In some cases these may be dead code. XK


- `SDL_MMIYOO_INPUT_MODE` -- `"keyboard"` / `"joystick"` (unset = joystick), live-switchable
- `SDL_MMIYOO_VSYNC_MODE` -- `"off"` (default, temporary decision - see comment on `SDL_HINT_MMIYOO_VSYNC_MODE` in `SDL_mmiyoo.h`) / `"adaptive"` / `"strict"` (real `/dev/l`-paced panning, launch-time only)
- `SDL_MMIYOO_DEBUG` / `SDL_MMIYOO_DEBUG_VERBOSE` -- raise driver log verbosity
- `SDL_MMIYOO_GEOMETRY_STATS` -- log per-present triangle/span stats
- `SDL_MMIYOO_GEOMETRY_BAND_HEIGHT` -- triangle rasterizer span band height (1-32, default 3)
- `SDL_VIDEO_MMIYOO_SAVE_FRAMES` -- dump each presented frame to a BMP

## Miyoo-specific functions

Sysfs write helper: `MMIYOO_WriteSysfs`
Integer file read helper: `MMIYOO_ReadIntFile`
Device model detection: `MMIYOO_GetDeviceModel`
Keycode to button-bitmask mapping: `MMIYOO_KeycodeToButtonMask`
Default framebuffer info: `MMIYOO_GetDefaultFramebufferInfo`
Framebuffer info from an open fd: `MMIYOO_GetFramebufferInfoFromFD`
Framebuffer metrics refresh: `MMIYOO_UpdateFramebufferMetrics`
Rumble capability probe: `MMIYOO_HasRumble`
Rumble GPIO control: `MMIYOO_SetRumble`
Rumble GPIO init: `MMIYOO_InitRumbleGPIO`
Battery percent probe: `MMIYOO_GetBatteryPercent`
Charging-state probe: `MMIYOO_IsCharging`
Battery percent from raw ADC: `MMIYOO_BatteryPercentFromADC`
Mini battery ADC read: `MMIYOO_ReadMiniBatteryADC`
Charging state via GPIO: `MMIYOO_IsChargingGPIO`
Charging/battery via axp_test: `MMIYOO_ReadAxpTest`
Shared /dev/input/event0 reader init/deinit: `MMIYOO_InputInit`, `MMIYOO_InputDeinit`
Shared input reader thread: `MMIYOO_InputThread`
Keypad bitmap snapshot: `MMIYOO_GetKeypadBitmap`
Keyboard/joystick mode getters: `MMIYOO_IsKeyboardModeActive`, `MMIYOO_IsJoystickModeActive`
VSync mode getter: `MMIYOO_GetVSyncMode`
Window focus restore: `MMIYOO_RaiseWindow`
Mouse bounds setter: `MMIYOO_SetMouseBounds`
Video driver availability check: `MMIYOO_Available`
Framebuffer clear: `FB_Clear`
Framebuffer init/teardown: `FB_Init`, `FB_Uninit`
Present (copy or real page-flip depending on vsync mode): `GFX_SwapBuffers`
GFX blit wrapper (blend/colormod/clip): `GFX_Copy`
Texture fence batching: `GFX_FlushTextureFences`, `GFX_AddTextureFence`
Framebuffer/double-buffer accessors: `GFX_GetFrameBuffer`, `GFX_GetFrameBufferVirtual`, `GFX_GetFrameStride`, `GFX_GetFrameWidth`, `GFX_GetFrameHeight`, `GFX_IsDoubleBuffered`, `GFX_IsPageFlipEnabled`
GLES default profile config: `MMIYOO_GLES_DefaultProfileConfig`
GLES buffer-settings extension hook: `glUpdateBufferSettings`
Sample-rate selection: `MMIYOO_SelectSampleRate`
Gamepad mapping: `MMIYOO_JoystickGetGamepadMapping`
Rumble auto-stop timer: `MMIYOO_HapticTimer`

## SDL driver functionality supported

**Video** (`SDL_VideoDevice`): `CreateDevice`, `DeleteDevice`, `VideoInit`, `VideoQuit`, `SetDisplayMode` (no-op), `CreateWindow`, `CreateWindowFrom`, `DestroyWindow`, `PumpEvents`, `CreateWindowFramebuffer`, `UpdateWindowFramebuffer`, `DestroyWindowFramebuffer`

**Render** (`SDL_Renderer`, driver name `MMIYOO`): `CreateRenderer`, `WindowEvent`, `CreateTexture`, `UpdateTexture`, `LockTexture`, `UnlockTexture`, `SetTextureScaleMode` (no-op), `SetRenderTarget`, `QueueSetViewport`, `QueueSetDrawColor`, `QueueDrawPoints`, `QueueDrawLines`, `QueueGeometry`, `QueueFillRects`, `QueueCopy`, `QueueCopyEx`, `RunCommandQueue`, `RenderPresent`, `DestroyTexture`, `DestroyRenderer`, `SetVSync`
Not supported: `RenderReadPixels`

**Audio** (`SDL_AudioDriverImpl`, driver name `MMIYOO`): `OpenDevice`, `WaitDevice`, `PlayDevice`, `GetDeviceBuf`, `CloseDevice`
Not supported: capture (`OnlyHasDefaultOutputDevice`)

**Joystick** (`SDL_JoystickDriver`): `Init`, `GetCount`, `Detect`, `GetDeviceName`, `GetDeviceGUID`, `GetDeviceInstanceID`, `Open`, `Update`, `Close`, `Quit`, `Rumble`, `GetCapabilities`, `GetGamepadMapping`
Not supported: `GetDevicePlayerIndex`/`SetDevicePlayerIndex` (stub), `RumbleTriggers`, `SetLED`, `SendEffect`, `SetSensorsEnabled`

**Haptic** (`SDL_SYS_Haptic*`): `Init`, `NumHaptics`, `HapticName`, `HapticOpen`, `HapticOpenFromJoystick`, `JoystickIsHaptic`, `JoystickSameHaptic`, `HapticClose`, `HapticQuit`, `HapticNewEffect`, `HapticUpdateEffect`, `HapticDestroyEffect`, `HapticRunEffect`, `HapticStopEffect`, `HapticStopAll`, `HapticGetEffectStatus`, `HapticPause`, `HapticUnpause`
Effect type supported: `SDL_HAPTIC_LEFTRIGHT` only
Not supported: `HapticMouse`, `SetAutocenter`

**Power** (`SDL_GetPowerInfo`): supported, via `SDL_GetPowerInfo_MMIYOO` -- no time-remaining estimate (`seconds` always -1)

**GLES** (`SDL_GLDriverData`, built only with `--enable-gles`): `glLoadLibrary`, `glGetProcAddress`, `glUnloadLibrary`, `glCreateContext`, `glMakeCurrent`, `glDeleteContext`, `glSwapWindow`, `glSetSwapInterval`, `glGetSwapInterval`
