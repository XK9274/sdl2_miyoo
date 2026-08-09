# Miyoo SDL Feature Summary

Brief summary of current `mmiyoo` backend support and the relevant functions/files.

## Build and Platform

- Miyoo platform configure path: `configure.ac` `CheckMMIYOO`
- Build script: `build-scripts/mk_miyoo.sh`
- Docker bootstrap: `--docker`
- Docker/toolchain rebuild: `--rebuild-container`
- External neon helper build/link: `libneonarmmiyoo` from `neon-arm-library-miyoo`
- Clean levels: `--clean build|deps|all`, `--clean-only build|deps|all`

## Shared Miyoo Core

- Core file: `src/core/mmiyoo/SDL_mmiyoo.c`
- Core header: `src/core/mmiyoo/SDL_mmiyoo.h`
- Sysfs write helper: `MMIYOO_WriteSysfs`
- Integer file read helper: `MMIYOO_ReadIntFile`
- Device model detection: `MMIYOO_GetDeviceModel`
- Linux keycode to Miyoo button bit mapping: `MMIYOO_KeycodeToButtonMask`
- Rumble capability probe: `MMIYOO_HasRumble`
- Rumble GPIO control: `MMIYOO_SetRumble`
- Battery percent probe: `MMIYOO_GetBatteryPercent`
- Charging-state probe: `MMIYOO_IsCharging`

## Video

- Video backend registration: `SDL_VIDEO_DRIVER_MMIYOO`
- Main video backend: `src/video/mmiyoo/SDL_video_mmiyoo.c`
- Framebuffer backend: `src/video/mmiyoo/SDL_framebuffer_mmiyoo.c`
- Event backend: `src/video/mmiyoo/SDL_event_mmiyoo.c`
- Event lifecycle: `MMIYOO_EventInit`, `MMIYOO_EventDeinit`
- Event pumping: `MMIYOO_PumpEvents`
- Mouse bounds helper: `MMIYOO_SetMouseBounds`
- Raw `/dev/input/event0` keyboard-style input is still supported by the video event backend.

## Rendering

- Render backend: `src/render/mmiyoo/SDL_render_mmiyoo.c`
- SDL renderer driver: `SDL_RenderDriver MMIYOO_RenderDriver`
- MI GFX accelerated rendering paths are present.
- MI SYS texture/memory tracking is present.
- Fence batching is present for MI GFX work.
- NEON helper integration is present for accelerated software/fallback work.
- Non-zero-angle `SDL_RenderCopyEx` remains a limited/fallback case and warns once.

## OpenGL ES

- GLES backend: `src/video/mmiyoo/SDL_opengles_mmiyoo.c`
- GLES header: `src/video/mmiyoo/SDL_opengles_mmiyoo.h`
- Build gates: `SDL_VIDEO_OPENGL_EGL` and `SDL_VIDEO_OPENGL_ES2`
- Default ES profile config: `MMIYOO_GLES_DefaultProfileConfig`
- Runtime EGL extension lookup is used for `eglUpdateBufferSettings`.
- Swap interval query support: `glGetSwapInterval`

## Audio

- Audio backend registration: `SDL_AUDIO_DRIVER_MMIYOO`
- Audio backend: `src/audio/mmiyoo/SDL_audio_mmiyoo.c`
- Dynamic sample-rate selection is supported.
- MI AO open/playback/close path is implemented.
- Close path clears channel buffer, disables channel/device, and clears public attributes.

## Joystick

- Joystick backend registration: `SDL_JOYSTICK_MMIYOO`
- Joystick backend: `src/joystick/mmiyoo/SDL_joystick_mmiyoo.c`
- Device count: `MMIYOO_JoystickGetCount` returns one synthetic joystick.
- Device name: `MMIYOO_JoystickGetDeviceName` returns `MMiyoo Joystick`.
- Device open: `MMIYOO_JoystickOpen`
- Poll/update: `MMIYOO_JoystickUpdate`
- Close/quit: `MMIYOO_JoystickClose`, `MMIYOO_JoystickQuit`
- Button events: `SDL_PrivateJoystickButton`
- Axis events: `SDL_PrivateJoystickAxis`
- D-pad is mirrored to axes 0 and 1.
- Auto gamepad mapping: `MMIYOO_JoystickGetGamepadMapping`
- Rumble through joystick API: `MMIYOO_JoystickRumble`
- Capabilities: `MMIYOO_JoystickGetCapabilities` reports `SDL_JOYCAP_RUMBLE` when GPIO rumble is available.

## Haptic / Rumble

- Haptic backend registration: `SDL_HAPTIC_MMIYOO`
- Haptic backend: `src/haptic/mmiyoo/SDL_syshaptic.c`
- Init/count/name: `SDL_SYS_HapticInit`, `SDL_SYS_NumHaptics`, `SDL_SYS_HapticName`
- Open/close: `SDL_SYS_HapticOpen`, `SDL_SYS_HapticClose`
- Joystick haptic bridge: `SDL_SYS_JoystickIsHaptic`, `SDL_SYS_HapticOpenFromJoystick`, `SDL_SYS_JoystickSameHaptic`
- Supported effect type: `SDL_HAPTIC_LEFTRIGHT`
- Effect lifecycle: `SDL_SYS_HapticNewEffect`, `SDL_SYS_HapticUpdateEffect`, `SDL_SYS_HapticDestroyEffect`
- Playback: `SDL_SYS_HapticRunEffect`, `SDL_SYS_HapticStopEffect`, `SDL_SYS_HapticStopAll`
- Status: `SDL_SYS_HapticGetEffectStatus`
- Pause/unpause: `SDL_SYS_HapticPause`, `SDL_SYS_HapticUnpause`

## Power

- Power backend registration: `SDL_POWER_MMIYOO`
- Power backend: `src/power/mmiyoo/SDL_syspower.c`
- SDL entry point served: `SDL_GetPowerInfo`
- Miyoo implementation: `SDL_GetPowerInfo_MMIYOO`
- Percent sources:
  - `/tmp/percBat`
  - Miyoo Mini Plus model 354 fallback via `/customer/app/axp_test`
  - Miyoo Mini ADC fallback via `/dev/sar`
- Charging sources:
  - Miyoo Mini Plus model 354 fallback via `/customer/app/axp_test`
  - GPIO59 fallback
- Reports `SDL_POWERSTATE_CHARGING`, `SDL_POWERSTATE_CHARGED`, `SDL_POWERSTATE_ON_BATTERY`, or falls through to unknown when no definitive source exists.

## Diagnostics

- Unsupported-operation callsite logging: `SDL_Unsupported_REAL`
- Hint: `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS`
- Renderer/window/texture magic diagnostics were added in core SDL paths.

## Known Future Work

- Verify joystick/gamecontroller/haptic/power on device with test apps.
- Decide whether Miyoo input should have one shared core event reader instead of separate video and joystick readers opening `/dev/input/event0`.
- Add benchmark support through a central helper/hook only.
- Detect active framebuffer geometry from `fbset` for devices with different screen sizes.
- Continue reviewing MI SYS allocation and texture memory handling in the renderer.
