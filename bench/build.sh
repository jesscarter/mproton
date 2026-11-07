#!/bin/sh
# Build mpbench for each Windows arch we care about. Output: bench/out/mpbench-<arch>.exe
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tc="$here/../toolchains/llvm-mingw/bin"
mkdir -p "$here/out"
for arch in x86_64 i686 aarch64 arm64ec; do
    "$tc/$arch-w64-mingw32-clang++" -O2 -std=c++17 -static -o "$here/out/mpbench-$arch.exe" \
        "$here/mpbench.cpp" -ld3d11 -ldxgi -ld3dcompiler
    echo "built out/mpbench-$arch.exe"
done
