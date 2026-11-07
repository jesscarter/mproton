#!/bin/bash
# Build upstream DXVK (D3D8/9/10/11 + DXGI on Vulkan) for mproton, reproducibly, with llvm-mingw.
#
#   graphics/dxvk/build.sh [--clean] [TARGET...]
#
# TARGETs (default: arm64x i386):
#   arm64x   64-bit DLLs for Apple Silicon: ARM64X images (ARM64EC + ARM64 halves in one file),
#            staged in aarch64-windows/. An x64 game, which runs as ARM64EC under FEX, loads the
#            ARM64EC half and so runs DXVK natively; a native ARM64 program loads the ARM64 half.
#   i386     32-bit DLLs, staged in i386-windows/. A 32-bit game runs under WoW64 + FEX and
#            DXVK is emulated with it.
#   x86_64   plain x86_64 DLLs in x86_64-windows/, for validating the same pin with x86_64 Wine
#            on Linux (scripts/validate-lavapipe.sh). Not shipped on macOS.
#
# DLLs: d3d8 d3d9 d3d10core d3d11 dxgi. Output: graphics/dxvk/out/dxvk-<describe>/<arch>-windows/,
# the layout of a Wine lib/wine dir, plus VERSION (git describe), COMMIT and PATCHES files.
#
# Version: dxvk.pin (tag + commit; the tag must resolve to the commit). Patches: every
# patches/*.patch is applied with `git apply` in name order (none at the moment; see
# patches/README.md). Applying any makes `git describe --dirty=+`, which DXVK records as its
# version, end in '+'.
#
# Environment:
#   LLVM_MINGW  llvm-mingw root (default: toolchains/llvm-mingw from scripts/setup.sh)
#   DXVK_WORK   source + build trees (default: graphics/dxvk/work)
#   DXVK_OUT    output root (default: graphics/dxvk/out)
#   WINEBUILD   if set, a winebuild to mark the staged copies as Wine builtins
#               (`winebuild --builtin`), which a bundle that installs them into lib/wine needs
#               (athei's mach9 does this for vkd3d-proton). Proton-style installs that copy the
#               DLLs into the prefix as native DLLs do not need it.
# Needs: git, meson, ninja, glslang (or glslangValidator), python3.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
MINGW_DIR=${LLVM_MINGW:-$root/toolchains/llvm-mingw}
WORK=${DXVK_WORK:-$here/work}
OUT_ROOT=${DXVK_OUT:-$here/out}
DLLS="d3d8 d3d9 d3d10core d3d11 dxgi"

clean=0
targets=()
for a in "$@"; do
    case $a in
        --clean) clean=1 ;;
        arm64x|i386|x86_64) targets+=("$a") ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown argument: $a (try --help)"; exit 2 ;;
    esac
done
[ ${#targets[@]} -gt 0 ] || targets=(arm64x i386)

# shellcheck source=dxvk.pin
. "$here/dxvk.pin"

# ── Tools ───────────────────────────────────────────────────────────────
for tool in git meson ninja python3; do
    command -v "$tool" >/dev/null || { echo "Error: $tool not found"; exit 1; }
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null \
    || { echo "Error: glslang not found (apt install glslang-tools / brew install glslang)"; exit 1; }
[ -x "$MINGW_DIR/bin/clang" ] || { echo "Error: no llvm-mingw at $MINGW_DIR (run scripts/setup.sh or set LLVM_MINGW)"; exit 1; }
# Appended, not prepended, so llvm-mingw's unprefixed clang does not become the native compiler.
# The cross files name tools without a path, so each must resolve into llvm-mingw, not into a
# distro mingw-w64 (whose gcc cannot target ARM64EC) earlier in PATH.
export PATH="$PATH:$MINGW_DIR/bin"
for t in "${targets[@]}"; do
    case $t in arm64x) triple=arm64ec-w64-mingw32 ;; i386) triple=i686-w64-mingw32 ;; x86_64) triple=x86_64-w64-mingw32 ;; esac
    for tool in gcc g++ ar strip windres; do
        if [ "$(command -v "$triple-$tool")" != "$MINGW_DIR/bin/$triple-$tool" ]; then
            echo "Error: $triple-$tool resolves to $(command -v "$triple-$tool" || echo nothing), not $MINGW_DIR/bin"
            exit 1
        fi
    done
done
READOBJ="$MINGW_DIR/bin/llvm-readobj"

# ── Source ──────────────────────────────────────────────────────────────
# A scratch clone forced to the pinned commit on every run. DXVK runs `git describe` for its
# version, so the tags are fetched too. Submodules (Vulkan/SPIR-V headers, dxbc-spirv,
# libdisplay-info) are at the commits the pinned tree records.
src="$WORK/src"
if [ "$clean" = 1 ]; then rm -rf "$WORK"; fi
mkdir -p "$WORK"
if [ ! -d "$src/.git" ]; then
    GIT_TERMINAL_PROMPT=0 git clone --quiet --no-checkout "$DXVK_REPO" "$src"
fi
GIT_TERMINAL_PROMPT=0 git -C "$src" fetch --quiet --force --tags "$DXVK_REPO" "refs/tags/$DXVK_TAG:refs/tags/$DXVK_TAG"
tag_commit=$(git -C "$src" rev-parse "refs/tags/$DXVK_TAG^{commit}")
if [ "$tag_commit" != "$DXVK_COMMIT" ]; then
    echo "Error: $DXVK_TAG is $tag_commit upstream, but dxvk.pin says $DXVK_COMMIT"
    exit 1
fi
git -C "$src" -c advice.detachedHead=false checkout --quiet --force --detach "$DXVK_COMMIT"
git -C "$src" clean -fdxq
GIT_TERMINAL_PROMPT=0 git -C "$src" submodule update --quiet --init --recursive --force
git -C "$src" submodule foreach --quiet --recursive 'git reset --quiet --hard && git clean -fdxq'
if git -C "$src" submodule status --recursive | grep '^[-+U]' >/dev/null; then
    echo "Error: DXVK submodules do not match $DXVK_COMMIT:"
    git -C "$src" submodule status --recursive | grep '^[-+U]'
    exit 1
fi

patches=()
for p in "$here"/patches/*.patch; do [ -f "$p" ] && patches+=("$p"); done
for p in ${patches[@]+"${patches[@]}"}; do
    echo "==> applying patches/$(basename "$p")"
    git -C "$src" apply --whitespace=nowarn "$p"
done
describe=$(git -C "$src" describe --tags --dirty=+)
echo "==> DXVK $describe ($DXVK_COMMIT), ${#patches[@]} patch(es)"

# ── Build ───────────────────────────────────────────────────────────────
# Each tree is reconfigured from scratch when the commit, the patches, the cross file or the
# compiler change, since a new compiler changes code without ninja noticing.
clang_ver=$("$MINGW_DIR/bin/clang" --version | head -n1)
patch_sum=$(cat /dev/null ${patches[@]+"${patches[@]}"} | sha256sum | cut -c1-16)
build_tree() {   # build_tree <target>
    local t=$1 dir="$WORK/build-$1" cross="$here/cross/llvm-mingw-$1.txt" key
    key="$DXVK_COMMIT patches=$patch_sum $(sha256sum < "$cross" | cut -c1-16) $clang_ver"
    if [ "$(cat "$dir/.setup-key" 2>/dev/null)" != "$key" ]; then
        rm -rf "$dir"
        meson setup "$dir" "$src" --cross-file "$cross" --buildtype release --strip \
            -Db_ndebug=if-release >/dev/null
        echo "$key" > "$dir/.setup-key"
    fi
    local tg=()
    for d in $DLLS; do
        case $d in d3d10core) tg+=("src/d3d10/d3d10core.dll") ;; *) tg+=("src/$d/$d.dll") ;; esac
    done
    echo "==> building $t"
    ninja -C "$dir" "${tg[@]}"
}
for t in "${targets[@]}"; do build_tree "$t"; done

# ── Stage + check ───────────────────────────────────────────────────────
pe_machine() {   # COFF Machine field: 8664 x86_64, 014c i386, aa64 ARM64 (ARM64X says aa64 too)
    local off; off=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
    od -An -tx2 -j$((off + 4)) -N2 "$1" | tr -d ' '
}
readobj_format() { "$READOBJ" --coff-load-config "$1" | sed -n 's/^Format: //p'; }
export_views() { "$READOBJ" --coff-exports "$1" | grep -cxE " *Name: $2" || true; }   # export tables naming <sym>

out="$OUT_ROOT/dxvk-$describe"
part="$out.part"
rm -rf "$part"
for t in "${targets[@]}"; do
    case $t in arm64x) dir=aarch64-windows ;; i386) dir=i386-windows ;; x86_64) dir=x86_64-windows ;; esac
    mkdir -p "$part/$dir"
    for d in $DLLS; do
        sub=$d; [ "$d" = d3d10core ] && sub=d3d10
        f="$part/$dir/$d.dll"
        cp "$WORK/build-$t/src/$sub/$d.dll" "$f"
        # One entry point per DLL that its users call: games call the Create functions, and
        # d3d10core/d3d8 reach d3d11/d3d9 through them.
        case $d in d3d8) sym=Direct3DCreate8 ;; d3d9) sym=Direct3DCreate9 ;; d3d10core) sym=D3D10CoreCreateDevice ;;
                   d3d11) sym=D3D11CreateDevice ;; dxgi) sym=CreateDXGIFactory1 ;; esac
        m=$(pe_machine "$f")
        case $t in
            arm64x)
                fmt=$(readobj_format "$f")
                [ "$m" = aa64 ] && [ "$fmt" = COFF-ARM64X ] \
                    || { echo "Error: $dir/$d.dll is not ARM64X (machine $m, format $fmt)"; exit 1; }
                views=2 ;;
            i386) [ "$m" = 014c ] || { echo "Error: $dir/$d.dll has machine $m, not 014c"; exit 1; }; views=1 ;;
            x86_64) [ "$m" = 8664 ] || { echo "Error: $dir/$d.dll has machine $m, not 8664"; exit 1; }; views=1 ;;
        esac
        n=$(export_views "$f" "$sym")
        [ "$n" = "$views" ] || { echo "Error: $dir/$d.dll exports $sym in $n of its $views views"; exit 1; }
        if [ -n "${WINEBUILD:-}" ]; then
            "$WINEBUILD" --builtin "$f"
            head -c 128 "$f" | grep -qa "Wine builtin DLL" || { echo "Error: $f not marked builtin"; exit 1; }
        fi
    done
    echo "    $dir: $DLLS ok ($([ $t = arm64x ] && echo "ARM64X, exports in both views" || echo "machine $m"))"
done
echo "$describe" > "$part/VERSION"
echo "$DXVK_COMMIT" > "$part/COMMIT"
: > "$part/PATCHES"
for p in ${patches[@]+"${patches[@]}"}; do echo "$(basename "$p") $(sha256sum < "$p" | cut -c1-64)" >> "$part/PATCHES"; done
cp "$here/dxvk.conf" "$part/dxvk.conf"
# Keep arch dirs of the same build that this run did not rebuild (e.g. `build.sh x86_64` after
# a default run), so one output dir can hold all three.
for d in "$out"/*-windows; do
    [ -d "$d" ] && [ ! -e "$part/$(basename "$d")" ] && cp -a "$d" "$part/"
done
rm -rf "$out"
mv "$part" "$out"
echo "==> staged $out"
