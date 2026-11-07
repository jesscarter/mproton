#!/bin/sh
# Build mpbench for each Windows arch we care about. Output: bench/out/mpbench-<arch>.exe
#   MPBENCH_ARCHS="x86_64 i686"   build a subset (default: x86_64 i686 aarch64 arm64ec)
#   LLVM_MINGW=/path/to/llvm-mingw override the toolchain (default: toolchains/llvm-mingw from scripts/setup.sh)
# Shaders are precompiled into mpbench_shaders.h; run gen_shaders.py (needs dxc) only after editing mpbench.hlsl.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tc="${LLVM_MINGW:-$here/../toolchains/llvm-mingw}/bin"
archs="${MPBENCH_ARCHS:-x86_64 i686 aarch64 arm64ec}"
mkdir -p "$here/out"

# If Vulkan headers are around (Linux distro package, Homebrew vulkan-headers, or
# VULKAN_HEADERS=<include dir>), check the hand-copied Vulkan subset against them.
# Only the vulkan/ directory is exposed, so host libc headers don't leak into the mingw build.
for d in "${VULKAN_HEADERS:-}" /usr/include /opt/homebrew/include /usr/local/include; do
    if [ -n "$d" ] && [ -f "$d/vulkan/vulkan_core.h" ]; then
        vkinc=$(mktemp -d); trap 'rm -rf "$vkinc"' EXIT
        ln -s "$d/vulkan" "$vkinc/vulkan"; [ -d "$d/vk_video" ] && ln -s "$d/vk_video" "$vkinc/vk_video"
        for arch in x86_64 i686; do
            "$tc/$arch-w64-mingw32-clang++" -std=c++17 -fsyntax-only -DMPBENCH_VK_CHECK -isystem "$vkinc" "$here/mpbench.cpp"
        done
        echo "vulkan subset matches $d/vulkan/vulkan_core.h"
        break
    fi
done

for arch in $archs; do
    "$tc/$arch-w64-mingw32-clang++" -O2 -std=c++17 -static -Wall -Wno-missing-field-initializers \
        -o "$here/out/mpbench-$arch.exe" "$here/mpbench.cpp"
    echo "built out/mpbench-$arch.exe"
done
