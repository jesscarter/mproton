#!/bin/sh
# Fetch untracked dev dependencies: llvm-mingw toolchain and upstream Proton reference clone.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
LLVM_MINGW_VER=20261104

mkdir -p "$root/toolchains" "$root/ref"
if [ ! -x "$root/toolchains/llvm-mingw/bin/clang" ]; then
    cd "$root/toolchains"
    tarball="llvm-mingw-$LLVM_MINGW_VER-ucrt-macos-universal.tar.xz"
    gh release download "$LLVM_MINGW_VER" -R mstorsjo/llvm-mingw -p "$tarball" --clobber
    tar xf "$tarball" && rm "$tarball"
    ln -sfn "llvm-mingw-$LLVM_MINGW_VER-ucrt-macos-universal" llvm-mingw
fi
if [ ! -d "$root/ref/proton" ]; then
    git clone --depth 1 https://github.com/ValveSoftware/Proton.git "$root/ref/proton"
fi
echo "setup done"
