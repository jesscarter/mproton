#!/bin/sh
# Correctness-only validation of mpbench on Linux: Wine + DXVK + vkd3d-proton on Mesa lavapipe
# (software Vulkan). It checks that every scene runs and writes sane JSON. The numbers measure a
# CPU rasterizer and say nothing about Mac performance.
#
# Usage: scripts/validate-lavapipe.sh DXVK_DIR VKD3D_PROTON_DIR [OUT_DIR]
#   DXVK_DIR / VKD3D_PROTON_DIR: extracted release tarballs (with x64/ and x86/ subdirs), or a
#   graphics/dxvk/build.sh output for DXVK (x86_64-windows/ and i386-windows/; build it with
#   `graphics/dxvk/build.sh x86_64 i386`).
#   Needs: wine (with wow64/32-bit support for i686), mesa-vulkan-drivers, and an X server
#   (DISPLAY set, or Xvfb on PATH; one is started on :99 if DISPLAY is empty).
#   Builds are taken from bench/out (run bench/build.sh first).
#   DXVK 3.1.1 needs a Wine whose winevulkan knows VK_KHR_load_store_op_none. Wine 9.0 (Ubuntu
#   24.04's) does not, so DXVK 3.1.1 reports "No adapters found" there; DXVK 2.7.1 still works.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
dxvk=$(cd "$1" && pwd); vkd3d=$(cd "$2" && pwd); out=${3:-$root/results}
mkdir -p "$out"; out=$(cd "$out" && pwd)   # mpbench runs from the work dir, so make it absolute
work=${MPBENCH_WORK:-$(mktemp -d)}; mkdir -p "$work"
frames=${MPBENCH_FRAMES:-120}

if [ -z "${DISPLAY:-}" ]; then
    Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 & export DISPLAY=:99; sleep 1
fi
export WINEPREFIX="$work/prefix" WINEDEBUG=-all DXVK_LOG_LEVEL=none VKD3D_DEBUG=none
export WINEDLLOVERRIDES="d3d11,dxgi,d3d10core,d3d12,d3d12core=n"
icd=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | tr '\n' ':'); export VK_DRIVER_FILES="${icd%:}"
mesa=$(env -u DISPLAY vulkaninfo --summary 2>/dev/null | sed -n 's/.*driverInfo *= *//p' | head -1)
wineboot -u >/dev/null 2>&1
# Xvfb reports a 0 Hz display mode through XRandR, which makes DXVK's dxgi divide by zero when
# creating a swapchain; Wine's non-XRandR display handler reports 60 Hz. Also stop winedbg from
# opening a crash dialog that would hang a headless run.
wine reg add 'HKCU\Software\Wine\X11 Driver' /v UseXRandR /t REG_SZ /d N /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
wineserver -w

wine_ver=$(wine --version)
note="CORRECTNESS ONLY: Linux, $wine_ver, DXVK $(basename "$dxvk"), $(basename "$vkd3d"), lavapipe ($mesa) software Vulkan under Xvfb. Numbers measure a CPU rasterizer, not Mac performance."

run() {   # run <pe arch> <dll subdir> <results name> [mpbench args...]
    arch=$1 dlls=$2 name=$3; shift 3
    dxvk_dlls=$dlls; [ "$dlls" = x86 ] && [ -d "$dxvk/x32" ] && dxvk_dlls=x32   # DXVK says x32, vkd3d-proton x86
    if [ -d "$dxvk/$arch-windows" ]; then dxvk_dlls=$arch-windows; fi           # graphics/dxvk/build.sh layout
    [ "$arch" = i686 ] && [ -d "$dxvk/i386-windows" ] && dxvk_dlls=i386-windows
    dir="$work/$arch"; mkdir -p "$dir"
    cp "$root/bench/out/mpbench-$arch.exe" "$dir/"
    cp "$dxvk/$dxvk_dlls/d3d11.dll" "$dxvk/$dxvk_dlls/dxgi.dll" "$dxvk/$dxvk_dlls/d3d10core.dll" "$vkd3d/$dlls/d3d12.dll" "$vkd3d/$dlls/d3d12core.dll" "$dir/"
    echo "== $name"
    (cd "$dir" && timeout 900 wine "mpbench-$arch.exe" --frames "$frames" --width 640 --height 360 \
        --note "$note" --out "$out/$name.json" "$@") || echo "mpbench exited with $?"
    wineserver -w
}

run x86_64 x64 linux-lavapipe-x86_64
run x86_64 x64 linux-lavapipe-x86_64-dxbc --no-cpu --api d3d12 --d3d12-shaders dxbc
run i686 x86 linux-lavapipe-i686
