#!/bin/bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<USAGE
Usage: $0 [options]

Options:
  --docker                    Clone the toolchain and build inside its Docker container
  --rebuild-container         Remove cached toolchain/image before the Docker build
  --strip | --no-strip        Control whether the final binaries are stripped (default: --strip)
  --enable-gles               Enable SDL's OpenGL ES backends during configure
  --disable-gles              Disable SDL's OpenGL ES backends (default)
  --skip-config               Skip the configure step (assumes an existing build tree)
  --skip-build                Skip the make step
  --skip-neon                 Do not build/copy the neon helper library
  --clean <level>             Clean before building, then continue: build, deps, or all
  --clean-only <level>        Clean and exit without building/toolchain: build, deps, or all
  --jobs <n>, -j <n>          Parallel build jobs for make (default: auto)
  --config-arg <arg>          Additional argument to pass to ./configure (may be repeated)
  --verbose                   Enable verbose shell output
  --help                      Show this help message
USAGE
}

# Defaults
USE_DOCKER=false
REBUILD_CONTAINER=false
STRIP_ENABLED=true
ENABLE_GLES=false
RUN_CONFIG=true
RUN_BUILD=true
BUILD_NEON=true
CLEAN_LEVEL=build
CLEAN_REQUESTED=false
CLEAN_ONLY=false
MAKE_JOBS=${MAKE_JOBS:-}
VERBOSE=false
CONFIGURE_EXTRA=()
PASSTHROUGH_ARGS=()
export MOD=mmiyoo

while [[ $# -gt 0 ]]; do
    case "$1" in
        --docker)
            USE_DOCKER=true
            ;;
        --rebuild-container)
            USE_DOCKER=true
            REBUILD_CONTAINER=true
            ;;
        --strip)
            STRIP_ENABLED=true
            PASSTHROUGH_ARGS+=(--strip)
            ;;
        --no-strip)
            STRIP_ENABLED=false
            PASSTHROUGH_ARGS+=(--no-strip)
            ;;
        --enable-gles)
            ENABLE_GLES=true
            PASSTHROUGH_ARGS+=(--enable-gles)
            ;;
        --disable-gles)
            ENABLE_GLES=false
            PASSTHROUGH_ARGS+=(--disable-gles)
            ;;
        --skip-config)
            RUN_CONFIG=false
            PASSTHROUGH_ARGS+=(--skip-config)
            ;;
        --skip-build)
            RUN_BUILD=false
            PASSTHROUGH_ARGS+=(--skip-build)
            ;;
        --skip-neon)
            BUILD_NEON=false
            PASSTHROUGH_ARGS+=(--skip-neon)
            ;;
        --clean)
            if [[ $# -lt 2 || "${2:0:1}" == "-" ]]; then
                echo -e "${RED}--clean requires a value: build, deps, or all${NC}" >&2
                usage
                exit 1
            fi
            shift
            case "$1" in
                build|deps|all)
                    CLEAN_LEVEL="$1"
                    CLEAN_REQUESTED=true
                    PASSTHROUGH_ARGS+=(--clean "$CLEAN_LEVEL")
                    ;;
                *)
                    echo -e "${RED}Unknown clean level: $1${NC}" >&2
                    usage
                    exit 1
                    ;;
            esac
            ;;
        --clean-only)
            if [[ $# -lt 2 || "${2:0:1}" == "-" ]]; then
                echo -e "${RED}--clean-only requires a value: build, deps, or all${NC}" >&2
                usage
                exit 1
            fi
            shift
            case "$1" in
                build|deps|all)
                    CLEAN_LEVEL="$1"
                    CLEAN_REQUESTED=true
                    CLEAN_ONLY=true
                    PASSTHROUGH_ARGS+=(--clean-only "$CLEAN_LEVEL")
                    ;;
                *)
                    echo -e "${RED}Unknown clean level: $1${NC}" >&2
                    usage
                    exit 1
                    ;;
            esac
            ;;
        --jobs|-j)
            shift || { echo -e "${RED}--jobs requires a value${NC}" >&2; exit 1; }
            MAKE_JOBS="$1"
            PASSTHROUGH_ARGS+=(--jobs "$MAKE_JOBS")
            ;;
        --config-arg)
            shift || { echo -e "${RED}--config-arg requires a value${NC}" >&2; exit 1; }
            CONFIGURE_EXTRA+=("$1")
            PASSTHROUGH_ARGS+=(--config-arg "$1")
            ;;
        --verbose)
            VERBOSE=true
            PASSTHROUGH_ARGS+=(--verbose)
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}" >&2
            usage
            exit 1
            ;;
    esac
    shift || true
done

if [[ "$USE_DOCKER" == true ]]; then
    TOOLCHAIN_REPO="https://github.com/XK9274/union-miyoomini-toolchain.git"
    TOOLCHAIN_CLONE_DIR="/tmp/union-miyoomini-toolchain"
    DOCKER_IMAGE="miyoomini-toolchain"

    if ! command -v docker >/dev/null 2>&1; then
        echo -e "${RED}Docker is not installed or not in PATH${NC}" >&2
        exit 1
    fi

    if ! docker info >/dev/null 2>&1; then
        echo -e "${RED}Docker daemon is not running${NC}" >&2
        exit 1
    fi

    if [[ "$REBUILD_CONTAINER" == true ]]; then
        echo -e "${YELLOW}Removing cached Docker image and toolchain clone...${NC}"

        if docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
            if ! docker image rm "$DOCKER_IMAGE"; then
                echo -e "${RED}Could not remove Docker image '$DOCKER_IMAGE'.${NC}" >&2
                echo -e "${YELLOW}If Docker requires elevated permissions, run:${NC}" >&2
                echo "  sudo docker image rm $DOCKER_IMAGE" >&2
                echo "Then rerun:" >&2
                echo "  $0 --docker --rebuild-container" >&2
                exit 1
            fi
        fi

        if [[ -d "$TOOLCHAIN_CLONE_DIR" ]]; then
            if ! rm -rf "$TOOLCHAIN_CLONE_DIR"; then
                echo -e "${RED}Could not remove cached toolchain clone at $TOOLCHAIN_CLONE_DIR.${NC}" >&2
                echo -e "${YELLOW}If it is owned by Docker/root, run:${NC}" >&2
                echo "  sudo rm -rf $TOOLCHAIN_CLONE_DIR" >&2
                echo "Then rerun:" >&2
                echo "  $0 --docker --rebuild-container" >&2
                exit 1
            fi
        fi
    fi

    if [[ ! -d "$TOOLCHAIN_CLONE_DIR/.git" ]]; then
        echo -e "${YELLOW}Cloning toolchain...${NC}"
        rm -rf "$TOOLCHAIN_CLONE_DIR"
        git clone --depth=1 "$TOOLCHAIN_REPO" "$TOOLCHAIN_CLONE_DIR"
    else
        echo -e "${GREEN}Toolchain already cloned at $TOOLCHAIN_CLONE_DIR${NC}"
    fi

    if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
        echo -e "${YELLOW}Building toolchain Docker image...${NC}"
        docker build -t "$DOCKER_IMAGE" "$TOOLCHAIN_CLONE_DIR"
    else
        echo -e "${GREEN}Toolchain image already built${NC}"
    fi

    echo -e "${YELLOW}Running build inside toolchain container...${NC}\n"
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        --workdir /workspace/sdl2 \
        -v "$SDL_DIR":/workspace/sdl2 \
        "$DOCKER_IMAGE" \
        /workspace/sdl2/build-scripts/mk_miyoo.sh "${PASSTHROUGH_ARGS[@]}"

    echo -e "\n${GREEN}Done. Output at: $SDL_DIR/output/libSDL2-2.0.so.0${NC}"
    exit 0
fi

if [[ -z "$MAKE_JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        MAKE_JOBS=$(nproc)
    else
        MAKE_JOBS=4
    fi
fi

[[ "$VERBOSE" == true ]] && set -x

OUTPUT_DIR="$SDL_DIR/output"
NEON_REPO="https://github.com/XK9274/neon-arm-library-miyoo.git"
NEON_CLONE_DIR="/tmp/neon-arm-library-miyoo"
TOOLCHAIN_ROOT="/opt/miyoomini-toolchain"

clean_build_tree() {
    local level="$1"

    echo -e "${YELLOW}Cleaning ${level} artifacts...${NC}"

    rm -f "$SDL_DIR"/Makefile \
        "$SDL_DIR"/SDL2.spec \
        "$SDL_DIR"/config.status \
        "$SDL_DIR"/config.cache \
        "$SDL_DIR"/config.log \
        "$SDL_DIR"/libtool \
        "$SDL_DIR"/sdl2-config \
        "$SDL_DIR"/sdl2.pc \
        "$SDL_DIR"/sdl2-config.cmake \
        "$SDL_DIR"/sdl2-config-version.cmake
    rm -rf "$SDL_DIR"/autom4te.cache "$SDL_DIR"/build

    if [[ "$level" == "deps" || "$level" == "all" ]]; then
        rm -f "$SDL_DIR"/include/neon.h \
            "$SDL_DIR"/libneonarmmiyoo.a \
            "$SDL_DIR"/libneonarmmiyoo.so
    fi

    if [[ "$level" == "all" ]]; then
        rm -rf "$OUTPUT_DIR"
    fi
}

VENDOR_GFX_INCLUDE_DIR="$SDL_DIR/build/vendor-include"
stage_patched_gfx_headers() {
    local toolchain_gfx_include="$TOOLCHAIN_ROOT/arm-linux-gnueabihf/libc/usr/include"

    if [[ ! -f "$toolchain_gfx_include/mi_gfx.h" || ! -f "$toolchain_gfx_include/mi_gfx_datatype.h" ]]; then
        echo -e "${RED}mi_gfx.h/mi_gfx_datatype.h not found under $toolchain_gfx_include${NC}" >&2
        exit 1
    fi

    mkdir -p "$VENDOR_GFX_INCLUDE_DIR"
    cp "$toolchain_gfx_include/mi_gfx.h" "$toolchain_gfx_include/mi_gfx_datatype.h" "$VENDOR_GFX_INCLUDE_DIR/"

    patch -p1 -s -d "$VENDOR_GFX_INCLUDE_DIR" < "$SDL_DIR/patches/mi_gfx.h.patch"
    patch -p1 -s -d "$VENDOR_GFX_INCLUDE_DIR" < "$SDL_DIR/patches/mi_gfx_datatype.h.patch"
}

if [[ "$CLEAN_REQUESTED" == true ]]; then
    clean_build_tree "$CLEAN_LEVEL"
fi

if [[ "$CLEAN_ONLY" == true ]]; then
    echo -e "${GREEN}Clean completed.${NC}"
    exit 0
fi

if [[ ! -d "$TOOLCHAIN_ROOT" ]]; then
    echo -e "${RED}miyoomini-toolchain not found at $TOOLCHAIN_ROOT${NC}" >&2
    echo -e "${YELLOW}Tip: run with --docker to bootstrap the toolchain automatically${NC}" >&2
    exit 1
fi

echo -e "${GREEN}====================================="
echo -e " SDL2 Build for Miyoo Mini"
echo -e " Source   : $SDL_DIR"
echo -e " Output   : $OUTPUT_DIR"
echo -e " Toolchain: $TOOLCHAIN_ROOT"
echo -e "=====================================${NC}\n"

export CROSS="$TOOLCHAIN_ROOT/bin/arm-linux-gnueabihf-"
export CC="${CROSS}gcc"
export CXX="${CROSS}g++"
export AR="${CROSS}ar"
export AS="${CROSS}as"
export LD="${CROSS}ld"
export HOST=arm-linux-gnueabihf
export PATH="$TOOLCHAIN_ROOT/bin:$PATH"

if [[ "$RUN_CONFIG" == true && "$CLEAN_REQUESTED" != true ]]; then
    clean_build_tree "$CLEAN_LEVEL"
fi

if [[ "$BUILD_NEON" == true ]]; then
    echo -e "${YELLOW}Building neon helper...${NC}"
    if [[ ! -d "$NEON_CLONE_DIR/.git" ]]; then
        rm -rf "$NEON_CLONE_DIR"
        git clone "$NEON_REPO" "$NEON_CLONE_DIR"
    else
        git -C "$NEON_CLONE_DIR" fetch --depth=1 origin
        git -C "$NEON_CLONE_DIR" reset --hard origin/HEAD
    fi

    make -C "$NEON_CLONE_DIR" clean >/dev/null 2>&1 || true
    make -C "$NEON_CLONE_DIR" CC="${CROSS}gcc" AR="${CROSS}ar" AS="${CROSS}as" LD="${CROSS}ld" CROSS_COMPILE="$CROSS"

    install -m 755 "$NEON_CLONE_DIR/lib/libneonarmmiyoo.so" "$SDL_DIR/libneonarmmiyoo.so"
    install -m 644 "$NEON_CLONE_DIR/lib/libneonarmmiyoo.a" "$SDL_DIR/libneonarmmiyoo.a"

    NEON_HEADER=""
    for candidate in \
        "$NEON_CLONE_DIR/include/neon.h" \
        "$NEON_CLONE_DIR/neon.h" \
        "$NEON_CLONE_DIR/src/neon.h"; do
        if [[ -f "$candidate" ]]; then
            NEON_HEADER="$candidate"
            break
        fi
    done

    if [[ -z "$NEON_HEADER" ]]; then
        echo -e "${RED}neon.h not found in $NEON_CLONE_DIR${NC}" >&2
        exit 1
    fi

    install -m 644 "$NEON_HEADER" "$SDL_DIR/include/neon.h"
else
    echo -e "${YELLOW}Skipping neon helper build as requested${NC}"
fi

pushd "$SDL_DIR" >/dev/null
chmod -R a+wx . >/dev/null 2>&1 || true

stage_patched_gfx_headers

if [[ "$RUN_CONFIG" == true ]]; then
    echo -e "${YELLOW}Running autogen/configure...${NC}"

    ./autogen.sh

    if [[ "$STRIP_ENABLED" == true ]]; then
        STRIP_FLAGS="-s"
        OPT_FLAGS="-O2"
        DEBUG_FLAGS=""
        unset STRIP
        echo "Strip enabled"
    else
        STRIP_FLAGS=""
        OPT_FLAGS="-O0"
        DEBUG_FLAGS="-g"
        export STRIP=":"
        echo "Strip disabled (debug build)"
    fi

    CONFIGURE_ARGS=(
        --disable-joystick-virtual
        --disable-alsa
        --disable-diskaudio
        --disable-video-x11
        --disable-video-wayland
        --disable-video-kmsdrm
        --disable-video-vulkan
        --disable-dbus
        --disable-ime
        --disable-fcitx
        --disable-hidapi
        --disable-pulseaudio
        --disable-sndio
        --disable-libudev
        --disable-jack
        --disable-video-opengl
        --disable-oss
        --disable-dummyaudio
        --disable-video-dummy
        --disable-debug
        --host=${HOST}
    )

    if [[ "$ENABLE_GLES" == true ]]; then
        CONFIGURE_ARGS+=(--enable-video-opengles --enable-video-opengles2)
    else
        CONFIGURE_ARGS+=(--disable-video-opengles --disable-video-opengles2)
    fi

    if [[ ${#CONFIGURE_EXTRA[@]} -gt 0 ]]; then
        CONFIGURE_ARGS+=("${CONFIGURE_EXTRA[@]}")
    fi

    CPPFLAGS="-DMMIYOO -I${VENDOR_GFX_INCLUDE_DIR} -I${SDL_DIR}/include -I${TOOLCHAIN_ROOT}/arm-linux-gnueabihf/libc/usr/include ${DEBUG_FLAGS}" \
    CFLAGS="${OPT_FLAGS} ${DEBUG_FLAGS}" \
    LDFLAGS="${STRIP_FLAGS} -L${SDL_DIR} -L${TOOLCHAIN_ROOT}/arm-linux-gnueabihf/libc/usr/lib" \
    LIBS="-lmi_sys -lmi_gfx -lmi_ao -lcam_os_wrapper -ldl ${LIBS:-}" \
    ./configure "${CONFIGURE_ARGS[@]}"
else
    echo -e "${YELLOW}Skipping configure step as requested${NC}"
fi

if [[ "$RUN_BUILD" == true ]]; then
    echo -e "${YELLOW}Building SDL2 (make -j${MAKE_JOBS})...${NC}"
    make -j"${MAKE_JOBS}" V=99
else
    echo -e "${YELLOW}Skipping build step as requested${NC}"
fi

popd >/dev/null

OUTPUT_LIB="$SDL_DIR/build/.libs/libSDL2-2.0.so.0.18.2"
if [[ -f "$OUTPUT_LIB" ]]; then
    mkdir -p "$OUTPUT_DIR"
    cp "$OUTPUT_LIB" "$OUTPUT_DIR/libSDL2-2.0.so.0"
    if [[ "$STRIP_ENABLED" == true ]]; then
        "${CROSS}strip" --strip-all "$OUTPUT_DIR/libSDL2-2.0.so.0"
    fi
    SIZE=$(stat -c%s "$OUTPUT_DIR/libSDL2-2.0.so.0")
    echo -e "${GREEN}Output : $OUTPUT_DIR/libSDL2-2.0.so.0 (${SIZE} bytes)${NC}"
else
    echo -e "${YELLOW}Warning: built library not found at $OUTPUT_LIB${NC}"
fi

echo -e "${GREEN}Build script completed.${NC}"
