Miyoo Mini
==========

SDL2 cross-compiled for the Miyoo Mini (arm-linux-gnueabihf, hard-float).
The Miyoo Mini runs on an SSD202D (Sigmastar/MStar) SoC with proprietary MI
(Media Interface) libraries for display, audio, and GFX acceleration.

> **AI disclosure:** there's been a substantial usage of various LLM in this
> project to both write the code & maintain the repo itself.

Changes
-------

Changes to this source weren't captured in a repo before 01/04/2026. See
[CHANGELOG.md](../CHANGELOG.md) for a rough overview of what's changed since
the vanilla branch, or browse the diff between `vanilla` and `new_miyoo`
directly.

Getting the library
-------------------

Pre-built releases are available on the Releases page. Each release attaches
a stripped `libSDL2-2.0.so.0` built against the `union-miyoomini-toolchain`.

Building from source
--------------------

The Miyoo build script is `build-scripts/mk_miyoo.sh`. It can be run from
anywhere inside or outside the repo. It requires `/opt/miyoomini-toolchain`
to be present, or use `--docker` to bootstrap it automatically.

Bootstrap without an installed toolchain:

```bash
./build-scripts/mk_miyoo.sh --docker --enable-gles
```

This clones `XK9274/union-miyoomini-toolchain`, builds the Docker image, and
runs the build inside the container. Toolchain and image are cached in `/tmp`
for subsequent runs.

Direct build with the toolchain already at `/opt/miyoomini-toolchain`:

```bash
./build-scripts/mk_miyoo.sh --enable-gles
```

The Miyoo build defaults to GLES/EGL disabled. Pass `--enable-gles` for the
normal Miyoo build; it configures SDL with both `--enable-video-opengles` and
`--enable-video-opengles2` and produces the EGL/GLES-enabled SDL backend. The
resulting `output/` directory includes the SDL library alongside the required
`libEGL.so` and `libGLESv2.so` runtime libraries.

Build options
-------------

| Flag | Default | Description |
|------|---------|-------------|
| `--docker` | off | Bootstrap toolchain via Docker and build inside it |
| `--rebuild-container` | off | Remove cached toolchain/image before the Docker build |
| `--strip` / `--no-strip` | `--strip` | Strip the output binary |
| `--enable-gles` / `--disable-gles` | `--disable-gles` | Enable OpenGL ES backends |
| `--skip-config` | off | Skip autogen/configure and reuse the existing build tree |
| `--skip-build` | off | Skip make |
| `--skip-neon` | off | Skip building the neon helper library |
| `--clean <level>` | off | Clean before building, then continue. `build` keeps releases, `deps` also removes generated neon files, `all` also removes releases |
| `--clean-only <level>` | off | Run the selected clean level and exit without requiring the toolchain |
| `--jobs <n>` / `-j <n>` | auto | Parallel make jobs |
| `--config-arg <arg>` | - | Extra argument passed to `./configure`; can be repeated |
| `--verbose` | off | Enable shell trace output with `set -x` |
| `--help` / `-h` | off | Show script help |

Runtime environment variables
------------------------------

| Variable | Default | Description |
|----------|---------|-------------|
| `SDL_MMIYOO_GLES_PRESENT_MODE` | `pbuffer` | GLES present strategy, builds with `--enable-gles` only. `pbuffer`: colour-correct, renders offscreen and hardware-blits to present. `windowsurface`: real EGL window surface via a vendor present extension -- kept for comparison only, has known colour corruption in translucent UI regions. See `docs/MMIYOO_SDL_FEATURE_SUMMARY.md`. |
| `SDL_MMIYOO_INPUT_MODE` | `joystick` | `keyboard` or `joystick`. Which backend posts SDL events from the shared raw-input reader; live app-switchable. |
| `SDL_MMIYOO_VSYNC_MODE` | `off` | `off`, `adaptive`, or `strict`. `strict` locks in real `/dev/l` panning double-buffering, read once at `FB_Init`; `off`/`adaptive` are read live every present. |
| `SDL_MMIYOO_INTEGER_SCALE` | on (`1`) | Set to `0` to disable the NEON integer-scale upscaler for core-content blits and fall back to unscaled-blit-only behavior. |
| `SDL_MMIYOO_TEXTURE_POOL` | on (`1`) | Set to `0` to disable the bounded MI_SYS MMA texture reuse pool. |
| `SDL_MMIYOO_TEXTURE_POOL_MAX_BYTES` | `10485760` (10 MiB) | Texture pool size cap in bytes. |
| `SDL_MMIYOO_GEOMETRY_QUICKPATH` | off | Enable glyph-quad duplicate-blit skip for textured geometry (e.g. font batches). |
| `SDL_MMIYOO_GEOMETRY_BAND_HEIGHT` | `3` | Software triangle rasterizer span-band height in pixels, clamped 1-32. |
| `SDL_MMIYOO_FRAME_TIMING` | off | Collect and log per-frame render timing stats. |
| `SDL_MMIYOO_GEOMETRY_STATS` | off | Collect and log geometry span stats. |
| `SDL_MMIYOO_DEBUG` | off | Raise the render-driver log category to debug priority. |
| `SDL_MMIYOO_DEBUG_VERBOSE` | off | Enable verbose render-driver debug logging (implied by `SDL_MMIYOO_DEBUG`). |

Acknowledgements
----------------

- [steward-fu](https://github.com/steward-fu) - original SDL2 port to the Miyoo Mini
- [shauninman](https://github.com/shauninman) - miyoomini-toolchain-buildroot,
  the cross-compilation toolchain this build targets
