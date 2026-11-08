#!/bin/sh
# Run displaymodes-<arch>.exe through the launcher with each D3D11 backend.
#   tests/displaymodes/run.sh [BUNDLE_DIR]   (default: <main checkout>/work/mach9/dist/wine)
#   DISPLAYMODES_ARCHS     default "x86_64 i686"
#   DISPLAYMODES_BACKENDS  default "dxmt dxvk wined3d" (dxmt = the launcher's default)
#   DISPLAYMODES_PREFIX    default ~/mproton-prefixes/displaymodes (a scratch prefix)
# Output: tests/displaymodes/out/<arch>-<backend>.txt
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
main=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)/..
bundle=${1:-$main/work/mach9/dist/wine}
prefix=${DISPLAYMODES_PREFIX:-$HOME/mproton-prefixes/displaymodes}
mkdir -p "$prefix"
for arch in ${DISPLAYMODES_ARCHS:-x86_64 i686}; do
    for be in ${DISPLAYMODES_BACKENDS:-dxmt dxvk wined3d}; do
        case $be in
            dxmt) set -- ;;
            dxvk) set -- PROTON_USE_DXVK=1 ;;
            wined3d) set -- PROTON_USE_WINED3D=1 ;;
        esac
        out=$here/out/$arch-$be.txt
        env MPROTON_WINE_DIR="$bundle" STEAM_COMPAT_DATA_PATH="$prefix" \
            STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/Library/Application Support/Steam}" \
            SteamAppId=0 SteamGameId=0 WINEDEBUG=-all "$@" \
            "$repo/launcher/mproton" waitforexitandrun "$here/out/displaymodes-$arch.exe" -v -o "Z:$(echo "$out" | tr / \\\\)" 2>"$out.err"
        echo "$arch $be: exit $? -> $out"
    done
done
