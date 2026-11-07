#!/bin/bash
# Reproduce the Proton-Wine x macOS-ARM64 patch triage (docs/wine-patch-triage.md).
#
# usage: wine-patches/scripts/reproduce.sh <workdir>
#
# Runs on Linux. Needs git, python3, autoconf, bison, flex, clang-20 + lld-20
# (Ubuntu 24.04: apt install clang-20 lld-20 llvm-20). Fetching takes ~2 GB.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mp=$(cd "$here/../.." && pwd)
work=${1:?usage: reproduce.sh <workdir>}
mkdir -p "$work" && cd "$work"
work=$(pwd)

# Pins (as triaged on 2026-11-07)
PROTON_WINE=e60892cc2398b0a83c332a2078c7b85e47bdb42b   # ValveSoftware/wine proton_11.0 (= Proton bc896217 submodule)
ATHEI=5959a04b1afe06ea1b119fa622f0b29f408ac8c4         # athei/wine arm64-patched (mach9 pins/wine-src.ref)
METALSHARP=1c97585b3db84bd7986da154f013d70002bf086b    # metalsharp/MetalSharp-Wine source/wine
WINEHQ_MASTER=78e52205d9b9ac1c7e6f55fc613b882f1ad093d9 # wine-11.21-164

if [ ! -d wine ]; then
    git clone -q --single-branch -b proton_11.0 https://github.com/ValveSoftware/wine wine
    git -C wine remote add winehq https://github.com/wine-mirror/wine   # gitlab.winehq.org mirror
    git -C wine remote add athei https://github.com/athei/wine
    git -C wine remote add metalsharp https://github.com/metalsharp/MetalSharp-Wine
    git -C wine fetch -q winehq --tags master
    git -C wine fetch -q athei arm64-patched
    git -C wine fetch -q metalsharp source/wine:refs/remotes/metalsharp/source-wine
fi
W=$work/wine
out=$work/results; mkdir -p "$out"
export TMPDIR=${TMPDIR:-$work/tmp}; mkdir -p "$TMPDIR"

# 1. Proton commits on top of wine-11.0, grouped and classified
python3 -I "$here/classify-proton.py" "$W" wine-11.0 $PROTON_WINE "$out" > "$out/proton-groups.txt"

# 2. Apply matrix: each macOS stack onto Proton 11, vanilla wine-11.0, wine-11.20 and WineHQ master
for base in proton:$PROTON_WINE wine11.0:wine-11.0 wine11.20:wine-11.20 master:$WINEHQ_MASTER; do
    name=${base%%:*}; ref=${base#*:}
    "$here/try-apply.sh" "$W" "$ref" wine-11.20 $ATHEI "$out/athei-on-$name.tsv"
    "$here/try-apply.sh" "$W" "$ref" wine-11.20 $ATHEI "$out/iso-athei-on-$name.tsv" isolated
    "$here/try-apply.sh" "$W" "$ref" wine-11.20 $METALSHARP "$out/ms-on-$name.tsv"
    "$here/try-apply.sh" "$W" "$ref" wine-11.20 $METALSHARP "$out/iso-ms-on-$name.tsv" isolated
done

# 3. The combined tree: Proton 11 + the 15 athei patches that can be carried (conflicts resolved by hand)
rm -rf "$work/comb"
git -C "$W" worktree add -q -B mproton/athei-on-proton "$work/comb" $PROTON_WINE
cd "$work/comb"
git -c user.name=triage -c user.email=triage@localhost am -q "$mp"/wine-patches/athei-on-proton-11.0/*.patch

# 4. Linux build check: configure + PE side for aarch64 and arm64ec with plain clang + lld.
#    Proton strips generated files from its tree; regenerate them as Proton's Makefile does.
export PATH=/usr/lib/llvm-20/bin:$PATH
autoreconf -fi
tools/make_requests
XDG_CACHE_HOME=$TMPDIR python3 dlls/winevulkan/make_vulkan -x vk.xml -X video.xml
tools/make_specfiles
mkdir -p "$work/build-comb" && cd "$work/build-comb"
../comb/configure --enable-archs=aarch64,arm64ec --with-mingw=clang --disable-tests \
    --without-x --without-wayland --without-gstreamer --without-vulkan --without-freetype > configure.log
make -k -j"$(nproc)" > make.log 2>&1 || true
grep -n 'error:' make.log | sort -u || echo "no compile errors"
