# SDL2 — Miyoo Mini

SDL2 cross-compiled for the Miyoo Mini (arm-linux-gnueabihf, hard-float). The Miyoo Mini runs on an SSD202D (Sigmastar/MStar) SoC with proprietary MI (Media Interface) libraries for display, audio, and GFX acceleration.

## Changes

Changes to this source weren't captured in a repo before 01/04/2026. See [CHANGELOG.md](CHANGELOG.md) for a rough overview of what's changed since the vanilla branch, or browse the [diff between `vanilla` and `new_miyoo`](../../compare/vanilla...new_miyoo) directly.

---

## Getting the library

Pre-built releases are available on the [Releases](../../releases) page. Each release attaches a stripped `libSDL2-2.0.so.0` built against the `union-miyoomini-toolchain`.

---

## Building from source

### Build script (`build-scripts/mk_miyoo.sh`)

Can be run from anywhere inside or outside the repo. Requires `/opt/miyoomini-toolchain` to be present, or use `--docker` to bootstrap it automatically.

**Bootstrap (no toolchain installed):**
```bash
./build-scripts/mk_miyoo.sh --docker
```
Clones [XK9274/union-miyoomini-toolchain](https://github.com/XK9274/union-miyoomini-toolchain), builds the Docker image, and runs the build inside the container. Toolchain and image are cached in `/tmp` for subsequent runs.

**Direct (toolchain already at `/opt/miyoomini-toolchain`):**
```bash
./build-scripts/mk_miyoo.sh
```

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--docker` | off | Bootstrap toolchain via Docker and build inside it |
| `--rebuild-container` | off | Remove cached toolchain/image before the Docker build |
| `--strip` / `--no-strip` | `--strip` | Strip the output binary |
| `--enable-gles` / `--disable-gles` | `--disable-gles` | Enable OpenGL ES backends |
| `--skip-config` | off | Skip autogen/configure (reuse existing build tree) |
| `--skip-build` | off | Skip make |
| `--skip-neon` | off | Skip building the neon helper library |
| `--clean <level>` | off | Clean before building, then continue: `build` keeps releases, `deps` also removes generated neon files, `all` also removes releases |
| `--clean-only <level>` | off | Run the selected clean level and exit without requiring the toolchain |
| `--jobs <n>` / `-j <n>` | auto | Parallel make jobs |
| `--config-arg <arg>` | — | Extra argument passed to `./configure` (repeatable) |
| `--verbose` | off | Enable shell trace output (`set -x`) |
| `--help` / `-h` | off | Show script help |

---

## Acknowledgements

- [steward-fu](https://github.com/steward-fu) — original SDL2 port to the Miyoo Mini
- [shauninman](https://github.com/shauninman) — miyoomini-toolchain-buildroot, the cross-compilation toolchain this build targets
