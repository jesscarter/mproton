#!/bin/bash
# Linux build check of a Wine source tree: native tools, then a cross build of the
# unix side for aarch64 Linux with PE arches aarch64, arm64ec and i386 (llvm-mingw).
#
# usage: wine/scripts/linux-build-check.sh <wine-src> <build-dir>
#
# Environment:
#   LLVM_MINGW   llvm-mingw root (default /opt/llvm-mingw); its clang also compiles the unix side
#   SYSROOT      aarch64 Linux sysroot with the -dev packages (default /opt/sysroot-arm64)
#   XBIN         dir with aarch64-linux-gnu-{ld,ar,ranlib,strip,nm} (default /opt/xbin); symlinks
#                to llvm-mingw's ld.lld/llvm-* work, as does binutils-aarch64-linux-gnu
#   JOBS         make -j (default nproc)
#   EXTRA_CONFIGURE  extra configure arguments
#
# On an aarch64 Linux host with the -dev packages installed, set NATIVE=1 to skip the
# cross setup (no --host, no sysroot).
set -eu
src=$(cd "${1:?usage: linux-build-check.sh <wine-src> <build-dir>}" && pwd)
build=${2:?usage: linux-build-check.sh <wine-src> <build-dir>}
mkdir -p "$build" && build=$(cd "$build" && pwd)
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
SYSROOT=${SYSROOT:-/opt/sysroot-arm64}
JOBS=${JOBS:-$(nproc)}
export PATH="${XBIN:-/opt/xbin}:$LLVM_MINGW/bin:$PATH"

common=(--disable-tests --enable-archs=aarch64,arm64ec,i386 --with-mingw)

if [ "${NATIVE:-0}" = 1 ]; then
    mkdir -p "$build/wine" && cd "$build/wine"
    "$src/configure" "${common[@]}" ${EXTRA_CONFIGURE:-} > configure.log 2>&1
else
    # 1. Native build tools (winebuild, widl, wrc, makedep, ...), from the same tree.
    mkdir -p "$build/tools" && cd "$build/tools"
    # (sfnt2fon needs FreeType: install the build host's libfreetype-dev)
    if [ ! -f tools/winebuild/winebuild ]; then
        "$src/configure" --enable-archs=x86_64 --without-mingw --disable-tests --without-x \
            --without-wayland --without-gstreamer --without-vulkan \
            --without-gnutls --without-sdl --without-pulse --without-alsa --without-dbus \
            --without-udev --without-usb --without-cups --without-krb5 --without-pcap \
            --without-unwind --without-opencl --without-v4l2 > configure.log 2>&1
        make -j"$JOBS" __tooldeps__ > make.log 2>&1
    fi
    # wrc looks for the NLS files next to its tools dir (<tools>/nls).
    mkdir -p nls && ln -sf "$src"/nls/*.nls nls/
    # 2. Cross build for aarch64 Linux.
    mkdir -p "$build/wine" && cd "$build/wine"
    # clang links through aarch64-linux-gnu-ld from XBIN.
    cc="clang --target=aarch64-linux-gnu --sysroot=$SYSROOT"
    PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="$SYSROOT" \
    "$src/configure" --host=aarch64-linux-gnu --with-wine-tools="$build/tools" "${common[@]}" \
        CC="$cc" CXX="clang++ --target=aarch64-linux-gnu --sysroot=$SYSROOT" \
        ${EXTRA_CONFIGURE:-} > configure.log 2>&1
fi
grep -E '^configure: (WARNING|error)' configure.log || true
grep -E '^(HOST_ARCH|PE_ARCHS|aarch64_CC|arm64ec_CC|i386_CC|UNIX_LIBS|DRM_LIBS) ' Makefile || true
make -k -j"$JOBS" > make.log 2>&1 && echo "BUILD OK" || echo "BUILD FAILED"
grep -E '(error|Error)[: ]' make.log | grep -v '^make.*Error [0-9]' | sort -u | head -100 || true
grep -E '^make.*Error [0-9]' make.log | sort -u | head -50 || true
