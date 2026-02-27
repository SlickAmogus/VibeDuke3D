#!/usr/bin/env bash
set -e

# All paths relative to this script's directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export NXDK_DIR=/c/Claude/nxdk
export PATH="${NXDK_DIR}/bin:$PATH"

# Prevent MSVC/Windows SDK include paths from leaking into nxdk-cc.
# vcvarsall.bat sets INCLUDE/LIB which clang picks up in MSVC compatibility
# mode, causing PC SDK headers (objidl.h, sysinfoapi.h, etc.) to conflict
# with nxdk's headers. nxdk-cc gets all needed paths via explicit -I flags.
unset INCLUDE LIB LIBPATH

# Compile xbox_startup.c manually from this shell (Git Bash) so NXDK_DIR
# propagates correctly to nxdk-cc. MSYS2 make loses NXDK_DIR in subprocesses.
NXDK_CFLAGS_COMMON="-DRENDERTYPESDL=1 -DUSE_POLYMOST=1 -DUSE_OPENGL=2 -DUSE_ASM=0 -D_XBOX=1 -DHAVE_SDL \
  -Ixbox_compat \
  -Isrc \
  -Ijfbuild/include \
  -Ijfbuild/src \
  -Ijfmact \
  -Ijfaudiolib/include \
  -Ijfaudiolib/src \
  -include xbox_compat/xbox_defs.h \
  -Wno-implicit-function-declaration -Wno-implicit-int -Wno-parentheses -Wno-dangling-else \
  -I${NXDK_DIR}/lib/winapi/winmm \
  -I${NXDK_DIR}/lib/usb/libusbohci/inc \
  -I${NXDK_DIR}/lib/usb/libusbohci_xbox/ \
  -DUSBH_USE_EXTERNAL_CONFIG='\"usbh_config_xbox.h\"' \
  -I${NXDK_DIR}/lib/zlib/zlib -DZ_SOLO \
  -I${NXDK_DIR}/lib/libpng/libpng -I${NXDK_DIR}/lib/libpng \
  -isystem ${NXDK_DIR}/lib/libjpeg/libjpeg-turbo -I${NXDK_DIR}/lib/libjpeg \
  -I${NXDK_DIR}/lib/net/lwip/src/include \
  -I${NXDK_DIR}/lib/net/nforceif/include \
  -I${NXDK_DIR}/lib/net/nvnetdrv \
  -I${NXDK_DIR}/lib/sdl/SDL2/include -DXBOX \
  -I${NXDK_DIR}/lib/sdl \
  -I${NXDK_DIR}/lib/sdl/SDL_ttf \
  -I${NXDK_DIR}/lib/sdl/SDL_ttf/external/freetype-2.4.12/include -DXBOX \
  -I${NXDK_DIR}/lib/sdl/SDL2_image \
  -DLOAD_BMP -DLOAD_GIF -DLOAD_JPG -DLOAD_LBM -DLOAD_PCX -DLOAD_PNG \
  -DLOAD_PNM -DLOAD_TGA -DLOAD_XCF -DLOAD_XPM -DLOAD_XV -DLOAD_XXX"

compile_if_stale() {
    local src="$1" obj="$2"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "xbox_compat/xbox_defs.h" -nt "$obj" ]; then
        echo "[ CC       ] $obj"
        eval nxdk-cc $NXDK_CFLAGS_COMMON \
            -MD -MP -MT "$obj" -MF "${obj%.obj}.c.d" \
            -c -o "$obj" "$src" 2>&1
    fi
}

compile_if_stale xbox_compat/xbox_startup.c xbox_compat/xbox_startup.obj
compile_if_stale xbox_compat/posix_io.c     xbox_compat/posix_io.obj

/c/msys64/usr/bin/make.exe -f Makefile.nxdk "$@" 2>&1
