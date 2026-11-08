#!/bin/sh
# Build displaymodes.exe for x86_64 and i686. Output: tests/displaymodes/out/displaymodes-<arch>.exe
#   LLVM_MINGW=/path/to/llvm-mingw   override the toolchain (default: toolchains/llvm-mingw)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
tc_root=${LLVM_MINGW:-$repo/toolchains/llvm-mingw}
# a git worktree has no toolchains/; use the main checkout's
[ -d "$tc_root" ] || tc_root=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)/../toolchains/llvm-mingw
mkdir -p "$here/out"
for arch in ${DISPLAYMODES_ARCHS:-x86_64 i686}; do
    "$tc_root/bin/$arch-w64-mingw32-clang++" -std=c++17 -O1 -static \
        -o "$here/out/displaymodes-$arch.exe" "$here/displaymodes.cpp" -ldxgi -ld3d11 -luser32
    echo "built out/displaymodes-$arch.exe"
done
