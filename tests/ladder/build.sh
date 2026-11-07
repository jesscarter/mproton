#!/bin/sh
# Build the ladder test programs for each Windows arch. Output: tests/ladder/out/<prog>-<arch>.exe
#   LADDER_ARCHS="x86_64 i686"     build a subset (default: aarch64 arm64ec x86_64 i686)
#   LADDER_PROGS="seh memory"      build a subset of programs (default: all in src/)
#   LLVM_MINGW=/path/to/llvm-mingw override the toolchain (default: toolchains/llvm-mingw from scripts/setup.sh)
#   VULKAN_HEADERS=<include dir>   where vulkan/vulkan_core.h is, to check vulkan.cpp's hand-copied
#                                  declarations (also looks in /usr/include and Homebrew)
# Every program is one .cpp plus src/ladder.h. Graphics APIs other than OpenGL are loaded with
# LoadLibrary at run time, so a program still starts (and reports SKIP) when a backend tree lacks
# a DLL, e.g. d3d12.dll in the wined3d tree.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tc="${LLVM_MINGW:-$here/../../toolchains/llvm-mingw}/bin"
archs="${LADDER_ARCHS:-aarch64 arm64ec x86_64 i686}"
progs="${LADDER_PROGS:-}"
[ -n "$progs" ] || progs=$(cd "$here/src" && ls ./*.cpp | sed 's|^\./||; s|\.cpp$||')
[ -x "$tc/x86_64-w64-mingw32-clang++" ] || { echo "build.sh: no llvm-mingw at $tc (run scripts/setup.sh)" >&2; exit 2; }
mkdir -p "$here/out"

# Check the Vulkan subset in vulkan.cpp against real headers when they are around.
case " $progs " in *" vulkan "*)
    for d in "${VULKAN_HEADERS:-}" /usr/include /opt/homebrew/include /usr/local/include; do
        if [ -n "$d" ] && [ -f "$d/vulkan/vulkan_core.h" ]; then
            vkinc=$(mktemp -d); trap 'rm -rf "$vkinc"' EXIT
            ln -s "$d/vulkan" "$vkinc/vulkan"; [ -d "$d/vk_video" ] && ln -s "$d/vk_video" "$vkinc/vk_video"
            "$tc/x86_64-w64-mingw32-clang++" -std=c++17 -fsyntax-only -fms-extensions -DLADDER_VK_CHECK -isystem "$vkinc" "$here/src/vulkan.cpp"
            "$tc/i686-w64-mingw32-clang++" -std=c++17 -fsyntax-only -DLADDER_VK_CHECK -isystem "$vkinc" "$here/src/vulkan.cpp"
            echo "vulkan subset matches $d/vulkan/vulkan_core.h"
            break
        fi
    done ;;
esac

for arch in $archs; do
    # __try/__except need -fms-extensions; i686 uses ladder.h's hand-built fs:[0] SEH instead.
    seh=-fms-extensions
    [ "$arch" = i686 ] && seh=
    for prog in $progs; do
        libs=""
        case $prog in
            callbacks | d3d9 | opengl) libs="-luser32 -lgdi32" ;;
        esac
        [ "$prog" = opengl ] && libs="$libs -lopengl32"
        # shellcheck disable=SC2086
        "$tc/$arch-w64-mingw32-clang++" -O2 -std=c++17 -static $seh -Wall -Wextra -Wno-unused-parameter \
            -Wno-unused-function -Wno-missing-field-initializers -o "$here/out/$prog-$arch.exe" "$here/src/$prog.cpp" $libs
    done
    echo "built $arch: $(echo $progs)"
done
