#!/usr/bin/env bash
set -e

export NXDK_DIR=/c/Claude/nxdk
export PATH="/c/Claude/nxdk/bin:$PATH"

# Prevent MSVC/Windows SDK include paths from leaking into nxdk-cc.
# vcvarsall.bat sets INCLUDE/LIB which clang picks up in MSVC compatibility
# mode, causing PC SDK headers (objidl.h, sysinfoapi.h, etc.) to conflict
# with nxdk's headers. nxdk-cc gets all needed paths via explicit -I flags.
unset INCLUDE LIB LIBPATH

cd /c/Claude/jfduke3d-xbox

# Compile xbox_startup.c manually from this shell (Git Bash) so NXDK_DIR
# propagates correctly to nxdk-cc. MSYS2 make loses NXDK_DIR in subprocesses.
NXDK_CFLAGS_COMMON="-DRENDERTYPESDL=1 -DUSE_POLYMOST=0 -DUSE_OPENGL=0 -DUSE_ASM=0 -DHAVE_SDL \
  -I/c/Claude/jfduke3d-xbox/xbox_compat \
  -I/c/Claude/jfduke3d-xbox/src \
  -I/c/Claude/jfduke3d-xbox/jfbuild/include \
  -I/c/Claude/jfduke3d-xbox/jfbuild/src \
  -I/c/Claude/jfduke3d-xbox/jfmact \
  -I/c/Claude/jfduke3d-xbox/jfaudiolib/include \
  -I/c/Claude/jfduke3d-xbox/jfaudiolib/src \
  -include /c/Claude/jfduke3d-xbox/xbox_compat/xbox_defs.h \
  -Wno-implicit-function-declaration -Wno-implicit-int -Wno-parentheses -Wno-dangling-else \
  -I/c/Claude/nxdk/lib/winapi/winmm \
  -I/c/Claude/nxdk/lib/usb/libusbohci/inc \
  -I/c/Claude/nxdk/lib/usb/libusbohci_xbox/ \
  -DUSBH_USE_EXTERNAL_CONFIG='\"usbh_config_xbox.h\"' \
  -I/c/Claude/nxdk/lib/zlib/zlib -DZ_SOLO \
  -I/c/Claude/nxdk/lib/libpng/libpng -I/c/Claude/nxdk/lib/libpng \
  -isystem /c/Claude/nxdk/lib/libjpeg/libjpeg-turbo -I/c/Claude/nxdk/lib/libjpeg \
  -I/c/Claude/nxdk/lib/net/lwip/src/include \
  -I/c/Claude/nxdk/lib/net/nforceif/include \
  -I/c/Claude/nxdk/lib/net/nvnetdrv \
  -I/c/Claude/nxdk/lib/sdl/SDL2/include -DXBOX \
  -I/c/Claude/nxdk/lib/sdl \
  -I/c/Claude/nxdk/lib/sdl/SDL_ttf \
  -I/c/Claude/nxdk/lib/sdl/SDL_ttf/external/freetype-2.4.12/include -DXBOX \
  -I/c/Claude/nxdk/lib/sdl/SDL2_image \
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
