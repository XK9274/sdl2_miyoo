# Session Handover — SDL2 Miyoo Mini Port

## Context

This document exists because the original development history for this SDL2 port was never in a git repo — work was done directly in a workspace directory (`sdl2_miyoo_vanilla`), changes accumulated without commits, and context was lost. The goal of this handover is to give the next agent enough information to write a meaningful release note that explains *why* changes were made, not just *what* changed.

---

## What this repo is

`XK9274/sdl2_miyoo` — a fork of SDL2 cross-compiled for the Miyoo Mini handheld (ARM Cortex-A7, hard-float, `arm-linux-gnueabihf`). The Miyoo Mini uses MStar/SigmaStar hardware with proprietary MI (Media Interface) libraries (`mi_sys`, `mi_gfx`, `mi_ao` etc.) for display, audio, and GPU access.

The `vanilla` branch is the upstream reference point. The `new_miyoo` branch contains all the changes described below.

---

## What this session did

This session was largely infrastructure work:

1. Removed `Copyright (C) 2024-2025 Matt W <matt@slipp.space>` from all Miyoo-specific source files
2. Cloned `sdl2_miyoo` from GitHub and created `new_miyoo` branch with the new source tree
3. Created GitHub Actions build workflow (`.github/workflows/build.yml`) — triggers on push/PR, installs toolchain from `shauninman/miyoomini-toolchain-buildroot` v0.0.3, builds neon helper lib, cross-compiles SDL2, strips and uploads artifact
4. Created GitHub Actions release workflow (`.github/workflows/release.yml`) — triggers on semver tags, runs preflight checks, builds, publishes GitHub release
5. Built and debugged both workflows locally via `nektos/act`
6. Created `build-scripts/mk_miyoo.sh` — standalone build script that works from anywhere in the source tree, includes `--docker` flag that bootstraps the entire toolchain via Docker automatically
7. Wrote `CHANGELOG.md` — mechanical diff of `new_miyoo` vs `vanilla`, but **missing the context of why changes were made** — this is what the next agent needs to fix
8. Wrote this `README.md` with three build methods documented

---

## The core problem for the next agent

`CHANGELOG.md` lists what changed but reads like a diff summary. It does not explain:
- Why `json-c` was removed
- Why `SDL_image` / `SDL_ttf` were removed
- Why the audio backend was rewritten
- Why the render backend gained fence batching
- Why EGL was refactored
- Why `SDL_Unsupported()` was extended

The next agent needs to **read the actual diffs**, reason about the context from the code itself, and produce a release note that a developer reading it would understand as a coherent narrative.

---

## The diff

Reference directories:
- **Vanilla (upstream)**: `/home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo_og` (branch: `vanilla`)
- **New (our work)**: `/home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo` (branch: `new_miyoo`)

To regenerate the diff:
```bash
diff -ru \
  --exclude='.git' --exclude='*.d' --exclude='*.o' --exclude='*.lo' \
  --exclude='*.la' --exclude='*.a' --exclude='*.so*' \
  --exclude='Makefile' --exclude='config.log' --exclude='config.status' \
  --exclude='sdl2-config' --exclude='sdl2.pc' --exclude='libtool' \
  --exclude='build' --exclude='output' \
  /home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo_og \
  /home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo
```

---

## Key changes and inferred context

### 1. Toolchain migration (`configure.ac`)

**Old**: Built against `/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot` — this was a different, older toolchain (mmiyoo). The host triple was `arm-linux`.

**New**: Built against `/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc` — the `union-miyoomini-toolchain` from `shauninman`, triple `arm-linux-gnueabihf`.

**Why**: The port was migrated from an older community toolchain (`mmiyoo`) to the `union-miyoomini-toolchain` which is the standard toolchain used by the Union ecosystem for Miyoo Mini. This is not just a path change — the sysroot layout differs, the library names differ, and the host triple is more specific. Everything downstream follows from this.

---

### 2. Removed `json-c`, `SDL_image`, `SDL_ttf` (`configure.ac`, `SDL_video_mmiyoo.c`, `SDL_video_mmiyoo.h`)

**Old**: Video backend linked `json-c`, `SDL_image`, `SDL_ttf` and included their headers directly. Had `hex_pen.h` for stylus input.

**New**: All removed. No JSON parsing in the video backend. No image/font library dependencies.

**Why**: In the old port, the video backend was doing application-level work — likely loading config files via JSON, loading images for overlays or UI elements, rendering text via TTF. This violated the principle that SDL backends should be thin hardware abstraction layers. The new architecture pushes all of that responsibility to the application (seed) layer. The Miyoo Mini port should provide framebuffer/MI access, not embed an app framework. `hex_pen.h` (stylus/pen digitiser) was removed because the Miyoo Mini does not have a touchscreen with a stylus — this was dead code carried from another platform variant.

Also relevant: `json-c` is not present in the `union-miyoomini-toolchain` sysroot by default, making it a hard dependency to satisfy at build time. Removing it simplifies cross-compilation significantly.

---

### 3. `nds_touch/` directory removed

**Why**: NDS (Nintendo DS) touch input was carried over from another device variant. The Miyoo Mini does not have an NDS-style dual-screen touch interface. Dead code removed.

---

### 4. Audio backend rewrite (`SDL_audio_mmiyoo.c/.h`)

**Old**: Hardcoded `MI_AUDIO_SAMPLE_PER_FRAME 768`. Static `stSetAttr`/`stGetAttr` structs. `CloseDevice` freed memory first, then disabled AO — a use-after-free risk. Linked `shmvar` and `mi_common` as required libraries.

**New**:
- `MMIYOO_SelectSampleRate()` dynamically finds the closest supported MI sample rate to whatever the application requests (supports 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 Hz)
- `CloseDevice` follows correct teardown order: `ClearChnBuf` → `DisableChn` → `Disable` → `ClrPubAttr`, all guarded by `ao_active` flag
- `shmvar` removed from link dependencies

**Why**: The hardcoded 768-frame buffer was causing audio issues with applications that request non-standard sample rates. The MI audio API supports a range of sample rates, and the old code would either fail or produce garbled audio when the requested rate didn't match. The teardown rewrite addresses a crash/hang seen when audio was closed mid-playback. `shmvar` is not available in the union toolchain sysroot.

---

### 5. Render backend major refactor (`SDL_render_mmiyoo.c`)

**Old**: Minimal render data struct. No batching. No NEON. Sparse texture data.

**New**:
- GFX fence batching (`pending_fences[512]`, `fence_count`, `batching_enabled`) — defers MI GFX fence waits and processes them in batches
- NEON acceleration paths via `neon.h` / `libneonarmmiyoo`
- Per-texture MI memory tracking (`phyAddr`, `virAddr`, `uses_msys_memory`, `gfx_surface`) for zero-copy GFX operations
- Viewport and clip rect state in render data
- `g_warned_copyex_angle` — suppresses repeated log spam when `SDL_RenderCopyEx` is called with non-zero rotation (not hardware-accelerated)
- `mmiyoo_texture_live_count` — leak detection

**Why**: The old renderer submitted each GFX operation synchronously and waited for the fence immediately — this caused a CPU stall on every blit. Batching fences allows multiple operations to be queued to the MI GFX hardware and waited on together, significantly improving throughput. The NEON paths via `libneonarmmiyoo` provide accelerated pixel copy/scale operations for cases where the MI GFX engine can't be used (e.g., software fallback paths). The per-texture MI memory tracking enables textures to be allocated in physically contiguous memory that the MI GFX engine can DMA from directly, avoiding a CPU copy on every render.

---

### 6. EGL refactor (`SDL_opengles_mmiyoo.c/.h`)

**Old**: EGL handles (`display`, `context`, `surface`, `config`) as bare file-scope globals. `eglUpdateBufferSettings` declared as an extern with a fixed signature. `glUnloadLibrary` called `eglTerminate` with `_this->gl_data->display` which was a raw cast — undefined behaviour if `gl_data` was NULL.

**New**:
- EGL handles moved into `SDL_GLDriverData` struct (one set of handles per SDL GL context, not global)
- `eglUpdateBufferSettings` resolved at runtime via `eglGetProcAddress` and cached as a typed function pointer
- `glUnloadLibrary` guards against NULL `gl_data` and resets handles to `EGL_NO_*` sentinels
- `MMIYOO_GLES_DefaultProfileConfig()` added to set ES2 profile as the default
- `glGetSwapInterval()` added

**Why**: The global EGL state was a correctness problem — if an application created and destroyed GL contexts multiple times (common in emulators that reinitialize SDL), the globals would become stale. Moving state into the driver data struct makes it per-context. `eglUpdateBufferSettings` is a Mstar extension not in the standard EGL headers; resolving it via `eglGetProcAddress` is the correct approach and avoids a link-time dependency on an undocumented symbol. The `glGetSwapInterval` addition is required by SDL's GL driver interface — its absence caused a linker warning or potentially a null function pointer call.

---

### 7. `SDL_Unsupported()` diagnostic extension (`SDL_error.h`, `src/SDL_error.c`, `SDL_hints.h`)

**Old**: `SDL_Unsupported()` was a macro that called `SDL_Error(SDL_UNSUPPORTED)` silently.

**New**: Macro expands to `SDL_Unsupported_REAL(__FILE__, __LINE__, SDL_FUNCTION)`. When `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS` is `"1"`, logs the call site to `SDL_LOG_CATEGORY_ERROR` at warn level.

**Why**: When porting applications to the Miyoo Mini backend, developers saw silent failures — operations would return -1 with `SDL_UNSUPPORTED` but there was no indication of which function triggered it or from where. This extension makes it trivial to enable a diagnostic mode that pinpoints exactly which SDL operations are hitting unimplemented backend paths, without modifying application code.

---

### 8. NULL guard improvements (`SDL_render.c`, `SDL_video.c`)

**New**: `CHECK_RENDERER_MAGIC`, `CHECK_TEXTURE_MAGIC`, `CHECK_WINDOW_MAGIC` all emit structured log lines with pointer values and magic values before returning error. `SDL_RenderCopyF`/`SDL_RenderCopyExF` return 0 (rather than asserting) when passed a NULL texture.

**Why**: Emulators and games running on the Miyoo Mini were producing `Invalid renderer` / `Invalid texture` errors with no diagnostic information, making them extremely hard to debug over a serial console or log file. The enhanced macros provide the pointer and magic values needed to distinguish a use-after-free from a wrong-context error. The NULL texture guard prevents a crash in games that conditionally render textures (checking for NULL at their level) but don't account for the SDL assert path.

---

### 9. Global rename `MMiyooVideoInfo` → `vid`

**Why**: `MMiyooVideoInfo` was a long name used in multiple translation units via extern. Renamed to `vid` for brevity in the new code. All references updated.

---

### 10. Centralised MI headers (`include/mi_*.h`)

**Old**: Each subsystem (`src/audio/mmiyoo/`, `src/video/mmiyoo/`) had its own copies of the MI headers.

**New**: All MI headers in `include/`. All includes in code updated. `SDL_video_mmiyoo.h` wraps MI includes in `#ifdef MMIYOO`.

**Why**: Header duplication was causing divergence — some copies had been locally modified, others hadn't. Centralising ensures a single source of truth for the MI API surface. The `#ifdef MMIYOO` guard ensures the headers don't interfere with non-Miyoo build configurations.

---

## Files changed summary

| File | Type of change |
|------|---------------|
| `configure.ac` | Toolchain migration, removed json-c/SDL_image/SDL_ttf/shmvar, added mi_sys/mi_gfx/mi_ao/neonarmmiyoo |
| `include/SDL_config.h` | Enabled dummy sensor backend |
| `include/SDL_error.h` | Extended `SDL_Unsupported()` with call-site capture |
| `include/SDL_hints.h` | Added `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS` |
| `include/mi_*.h` | Added — centralised MI headers (previously scattered per-subsystem) |
| `src/SDL_error.c` | Implemented `SDL_Unsupported_REAL()` |
| `src/render/SDL_render.c` | NULL guards, diagnostic macros |
| `src/render/mmiyoo/SDL_render_mmiyoo.c` | Fence batching, NEON, per-texture MI memory, major refactor |
| `src/audio/mmiyoo/SDL_audio_mmiyoo.c` | Dynamic sample rate, proper teardown, ao_active tracking |
| `src/audio/mmiyoo/SDL_audio_mmiyoo.h` | Added frame/ao_active fields, mi_ao_datatype include |
| `src/video/SDL_video.c` | Diagnostic window magic, NULL guard in SetWindowTitle |
| `src/video/mmiyoo/SDL_video_mmiyoo.c` | json-c removed, global rename, framebuffer helpers, crash diagnostics |
| `src/video/mmiyoo/SDL_video_mmiyoo.h` | Removed SDL_ttf/SDL_image, added threading, centralised MI includes |
| `src/video/mmiyoo/SDL_opengles_mmiyoo.c` | EGL per-context state, runtime function pointer, proper teardown |
| `src/video/mmiyoo/SDL_opengles_mmiyoo.h` | Added config/swap_interval fields, glGetSwapInterval, DefaultProfileConfig |
| `src/video/mmiyoo/SDL_event_mmiyoo.c` | Global rename vid.window |
| `.github/workflows/build.yml` | New — CI build workflow |
| `.github/workflows/release.yml` | New — CI release workflow |
| `build-scripts/mk_miyoo.sh` | New — standalone build script with Docker bootstrap |
| `README.md` | Replaced with build instructions |
| `CHANGELOG.md` | Written this session — mechanical, lacks context |

---

## What the next agent should do

1. Read this document in full
2. Read `CHANGELOG.md` (the mechanical diff summary at `/home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo/CHANGELOG.md`)
3. Use the context in sections above to **rewrite `CHANGELOG.md`** as a proper release note — developer-readable, grouped by concern, explaining the *why* behind each change
4. The tone should be: this is a changelog for a developer who uses this SDL2 fork and wants to know what improved, what was fixed, and what they may need to update in their own code

The diff source is at `/home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo_og` (vanilla) vs `/home/mattpc/HueTesting/union-miyoomini-toolchain/workspace/sdl2_miyoo` (new_miyoo). The agent can re-read any specific file to verify context before writing.
