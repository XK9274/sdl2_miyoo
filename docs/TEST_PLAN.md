# Test Plan — new_miyoo → PR to main

---

## Phase 1 — Pre-flight source checks
*Do these before attempting any build. Catch obvious issues first.*

- [ ] No remaining `vid` references in source — `grep -rn "\bvid\b" src/video/mmiyoo/ src/audio/mmiyoo/`
- [ ] `SDL_Unsupported_REAL` compiles with the hint-check block commented out (no unreferenced variable warnings)
- [ ] No `printf` / `fflush` left in non-error paths in `SDL_render_mmiyoo.c` — the success printf in `CreateTexture` was removed, verify it's gone
- [ ] No stale build artifacts committed — `Makefile`, `config.status`, `config.log`, `build/` should not be in the index
- [ ] `mmiyoo_debug_verbose` is fully commented out — no compiler warning for unused variable

---

## Phase 2 — Build script (`build-scripts/mk_miyoo.sh`)
*Requires toolchain installed at `/opt/miyoomini-toolchain` or Docker for the `--docker` path.*

**Direct build:**
- [ ] `./build-scripts/mk_miyoo.sh` completes without error
- [ ] `output/libSDL2-2.0.so.0` exists after build
- [ ] `--no-strip` produces a larger unstripped binary with debug sections
- [ ] `--skip-neon` completes without attempting to clone the neon library
- [ ] `--jobs 2` passes `-j2` to make correctly
- [ ] `--enable-gles` configure output shows OpenGL ES backends enabled
- [ ] `--config-arg --disable-shared` is passed through to configure correctly
- [ ] `--verbose` enables `set -x` trace output

**Docker build:**
- [ ] `./build-scripts/mk_miyoo.sh --docker` clones toolchain to `/tmp/union-miyoomini-toolchain` and builds Docker image
- [ ] Second run uses cached clone and cached image (no re-clone, no re-build of image)
- [ ] `output/libSDL2-2.0.so.0` exists on the host after the container exits
- [ ] `./build-scripts/mk_miyoo.sh --docker --no-strip` — verify `--no-strip` is passed through to the container
- [ ] `./build-scripts/mk_miyoo.sh --docker --jobs 4` — verify `--jobs 4` reaches the container build

---

## Phase 3 — Output binary validation
*Run against the stripped output from either Phase 2 path.*

- [ ] `file output/libSDL2-2.0.so.0` → `ELF 32-bit LSB shared object, ARM, ... stripped`
- [ ] `arm-linux-gnueabihf-readelf -h output/libSDL2-2.0.so.0` → Machine: ARM, Flags include hard-float ABI (`0x5000400`)
- [ ] `arm-linux-gnueabihf-readelf -d output/libSDL2-2.0.so.0 | grep NEEDED` → runtime deps only (libc, libm, libdl, libpthread) — MI libs load at runtime on-device
- [ ] Size in expected range (~1.1MB stripped, ~5–8MB unstripped)
- [ ] No x86 symbols present — `arm-linux-gnueabihf-nm output/libSDL2-2.0.so.0` should show ARM symbols only

---

## Phase 4 — GitHub Actions (local via `act`)
*Tests the CI workflow without pushing to remote.*

```bash
act push \
  -W .github/workflows/build.yml \
  -P ubuntu-latest=catthehacker/ubuntu:act-22.04 \
  --artifact-server-path /tmp/act-artifacts \
  --bind
```

- [ ] All steps complete without error
- [ ] Build info step prints correct arch, size, and dynamic dependencies
- [ ] Upload artifact step is skipped (`ACT=true` gate working)
- [ ] Output deposited to `output/libSDL2-2.0.so.0` in repo directory

---

## Phase 5 — GitHub Actions (remote)
*Push the branch to trigger real CI.*

- [ ] Push `new_miyoo` to remote — `build.yml` triggers automatically
- [ ] All steps pass in the Actions tab
- [ ] `libSDL2-miyoo-<sha>` artifact is attached to the workflow run and downloadable
- [ ] `release.yml` does **not** trigger (no tag pushed)
- [ ] Open a draft PR from `new_miyoo` → `main` — verify `build.yml` triggers on the PR event

---

## Phase 6 — Release workflow
*Test the full release pipeline before the real PR merge.*

- [ ] Push a test tag: `git tag v2.0.22-miyoo1-test && git push origin v2.0.22-miyoo1-test`
- [ ] Preflight job passes: semver format check, clean tree check, no duplicate release check, `autogen.sh` present, `build-scripts/mk_miyoo.sh` present
- [ ] Build job completes and artifact is produced
- [ ] GitHub release is created with `libSDL2-2.0.so.0` attached
- [ ] Delete the test release and tag afterwards

---

## Phase 7 — Device testing (on Miyoo Mini)
*Pull the `.so` from the release artifact and test on hardware.*

- [ ] Library loads — `LD_LIBRARY_PATH=. SDL_VIDEODRIVER=mmiyoo ./sdl_test_app` exits cleanly
- [ ] `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)` succeeds
- [ ] Window creation: `SDL_CreateWindow` returns non-NULL
- [ ] Basic render cycle: `SDL_RenderClear` + `SDL_RenderPresent` with no crash
- [ ] Texture create/destroy: create 10 textures, destroy all, no visible leak or hang
- [ ] Audio at 44100 Hz: opens and plays without garbled output
- [ ] Audio at 22050 Hz: `MMIYOO_SelectSampleRate` normalises correctly, no MI API error
- [ ] Audio at 11025 Hz: same
- [ ] Audio close mid-playback: no crash or hang
- [ ] EGL context: create, make current, swap, destroy — no crash
- [ ] EGL context create/destroy twice in sequence — no stale global state
- [ ] `SDL_HINT_REPORT_UNSUPPORTED_OPERATIONS=1` set before init — no crash, accepted silently

---

## Phase 8 — Pre-PR checklist
*Final checks before raising the real PR.*

- [ ] All Phase 1–7 items above are green
- [ ] `CHANGELOG.md` accurately reflects the diff — no items that were reverted
- [ ] `README.md` links are valid: Releases page, diff link, CHANGELOG.md
- [ ] No `.d`, `.o`, `.la`, `Makefile`, `config.status` files tracked in git
- [ ] Branch is up to date with or rebased onto `main`
- [ ] PR description references the CHANGELOG and the vanilla diff link
