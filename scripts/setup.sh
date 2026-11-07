#!/bin/sh
# Fetch untracked dev dependencies: llvm-mingw toolchain and upstream Proton reference clone.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
LLVM_MINGW_VER=20261104

mkdir -p "$root/toolchains" "$root/ref"
if [ ! -x "$root/toolchains/llvm-mingw/bin/clang" ]; then
    cd "$root/toolchains"
    case "$(uname -s)-$(uname -m)" in
        Darwin-*) flavour=macos-universal ;;
        Linux-x86_64) flavour=ubuntu-22.04-x86_64 ;;
        Linux-aarch64) flavour=ubuntu-22.04-aarch64 ;;
        *) echo "no llvm-mingw build for $(uname -s)-$(uname -m)"; exit 1 ;;
    esac
    name="llvm-mingw-$LLVM_MINGW_VER-ucrt-$flavour"
    gh release download "$LLVM_MINGW_VER" -R mstorsjo/llvm-mingw -p "$name.tar.xz" --clobber
    tar xf "$name.tar.xz" && rm "$name.tar.xz"
    ln -sfn "$name" llvm-mingw
fi
if [ ! -d "$root/ref/proton" ]; then
    git clone --depth 1 https://github.com/ValveSoftware/Proton.git "$root/ref/proton"
fi
echo "setup done"
