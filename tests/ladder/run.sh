#!/bin/bash
# The mproton correctness ladder: run every tests/ladder program against one Wine bundle and
# write results/ladder-<date>/summary.md with a pass/fail matrix.
#
# Usage: tests/ladder/run.sh BUNDLE_DIR
#   BUNDLE_DIR  the ARM64 Wine bundle (bin/wine, bin/wineserver, lib/wine/dxgi/<impl>/ and
#               lib/wine/d3d9/<impl>/ trees selected by compatdb rules), or any Wine install
#               with bin/wine (e.g. /usr on Linux), or "system" for the wine on PATH.
#
# Rungs, in order (every run has a timeout; a crash or hang is recorded and the ladder goes on):
#   1. setup       fresh prefix, wineboot -u, wineserver -w (the prefix boots and the server exits)
#   2. serial      every program for every arch. Graphics programs once per backend:
#                    d3d11, d3d12  each lib/wine/dxgi/<impl> in LADDER_BACKENDS (rule dxgi=<impl>)
#                    d3d9          each lib/wine/d3d9/<impl> (rule d3d9=<impl>)
#                    opengl        each of LADDER_GL_BACKENDS (rule opengl=<value>)
#                    vulkan        once
#                  The compatdb block each process logs is checked: a run that got another tree
#                  than its rule asked for FAILs as a backend mismatch. A Wine without compatdb
#                  trees (Linux) runs each graphics program once on its built-in stack, plus DXVK
#                  if LADDER_DXVK_DIR is set.
#   3. concurrent  all archs at once, each running every program, in ONE wineserver lifetime (a
#                  sentinel process holds the server; it must still be alive at the end)
#   4. tso         the ordering suite again for the x86 builds with FEX_HARDWARE_TSO=1 and =0
#                  (ARM hosts only; on an x86 host there is no FEX)
#   5. heavy       optional real-world rungs, see LADDER_HEAVY below
#   6. shutdown    wineserver -w must return on its own (no lingering processes), then
#                  wineserver -k, and the server socket must be gone
#
# Environment:
#   LADDER_ARCHS        default "aarch64 arm64ec x86_64 i686" on ARM hosts, "x86_64 i686" elsewhere
#   LADDER_PROGS        CPU programs, default "threads seh callbacks memory ordering"
#   LADDER_GFX          graphics programs, default "d3d9 d3d11 d3d12 opengl vulkan"
#   LADDER_BACKENDS     dxgi trees to try, default "wined3d dxmt dxvk" (missing trees are skipped)
#   LADDER_D3D9         d3d9 trees, default: every directory under lib/wine/d3d9/
#   LADDER_GL_BACKENDS  compatdb opengl values, default "egl cgl" (bundles with compatdb only)
#   LADDER_DXVK_DIR     non-bundle Wine: a DXVK release (x64/ x32/) to also run d3d9/d3d11 on
#   LADDER_TIMEOUT      seconds per program run (default 300)
#   LADDER_ORDER_ITERS  --iters for the ordering suite (default: the program's 500000)
#   LADDER_PHASES       default "setup serial concurrent tso heavy shutdown"
#   LADDER_HEAVY        space-separated optional rungs (default none):
#                         mono      C# hello world on Wine Mono (src/heavy/hello.cs). Needs a C#
#                                   compiler on the host (mcs, csc or dotnet's csc) or
#                                   LADDER_MONO_EXE=<prebuilt .exe>, and Wine Mono in the prefix.
#                         chromium  Windows Chromium headless (--dump-dom of a page whose JS
#                                   computes a marker), which starts browser, renderer, GPU and
#                                   utility subprocesses and runs V8's JIT under FEX. Needs
#                                   LADDER_CHROMIUM=<path to chrome.exe> from a Win_x64 snapshot,
#                                   e.g. chromium-browser-snapshots/Win_x64/<rev>/chrome-win.zip.
#                       The real target behind the chromium rung is the Windows Steam client's
#                       steamwebhelper.exe (CEF). That needs a Steam login, so it is not automated.
#   LADDER_DATE         default today; output goes to results/ladder-<date>/
#   LADDER_OUT          override the output directory
#   LADDER_PREFIX       override the prefix (default prefixes/ladder-<date>, gitignored)
#   LADDER_BIN          the built programs (default tests/ladder/out, from tests/ladder/build.sh)
#
# Exit status: 0 when every FAIL is listed in tests/ladder/known-failures.txt, 1 otherwise.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
[ $# -eq 1 ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
python=${PYTHON:-python3}
die() { echo "ladder: $*" >&2; exit 2; }

# ---- inputs -----------------------------------------------------------------------------------
if [ "$1" = system ]; then
    wine=$(command -v wine) || die "no wine on PATH"
    bundle=""
else
    bundle=$(cd "$1" 2>/dev/null && pwd) || die "no directory $1"
    wine=$bundle/bin/wine
    [ -x "$wine" ] || die "$bundle has no bin/wine"
fi
wineserver=$(dirname "$wine")/wineserver
[ -x "$wineserver" ] || wineserver=$(command -v wineserver) || die "no wineserver next to $wine"
compatdb=0
[ -n "$bundle" ] && [ -d "$bundle/lib/wine/dxgi" ] && compatdb=1

case $(uname -m) in arm64 | aarch64) host_arm=1 ;; *) host_arm=0 ;; esac
host_arm=${LADDER_HOST_ARM:-$host_arm}   # tests set this to exercise the ARM-only phases
default_archs="x86_64 i686"
[ "$host_arm" = 1 ] && default_archs="aarch64 arm64ec x86_64 i686"
archs=${LADDER_ARCHS:-$default_archs}
progs=${LADDER_PROGS:-threads seh callbacks memory ordering}
gfx=${LADDER_GFX:-d3d9 d3d11 d3d12 opengl vulkan}
backends=${LADDER_BACKENDS:-wined3d dxmt dxvk}
gl_backends=${LADDER_GL_BACKENDS:-egl cgl}
limit=${LADDER_TIMEOUT:-300}
phases=${LADDER_PHASES:-setup serial concurrent tso heavy shutdown}
heavy=${LADDER_HEAVY:-}
bin=${LADDER_BIN:-$here/out}
date=${LADDER_DATE:-$(date +%F)}
out=${LADDER_OUT:-$root/results/ladder-$date}
prefix=${LADDER_PREFIX:-$root/prefixes/ladder-$date}

have_any=0
for a in $archs; do [ -f "$bin/threads-$a.exe" ] && have_any=1; done
[ "$have_any" = 1 ] || die "no programs in $bin (run tests/ladder/build.sh)"

rm -rf "$out"
mkdir -p "$out/logs"
work=$(mktemp -d "${TMPDIR:-/tmp}/ladder.XXXXXX")
trap 'rm -rf "$work"; [ -n "${xvfb_pid:-}" ] && kill "$xvfb_pid" 2>/dev/null' EXIT
runs=$out/runs.tsv
printf 'phase\tarch\tprog\tbackend\ttso\tstatus\texit\tseconds\tlog\trule\n' >"$runs"

# A headless Linux box needs an X server for the window/GL/D3D9 programs.
if [ "$(uname -s)" = Linux ] && [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null; then
    Xvfb :97 -screen 0 1280x1024x24 >/dev/null 2>&1 &
    xvfb_pid=$!
    export DISPLAY=:97
    sleep 1
fi

# run_limited SECS LOG CMD...: run CMD (stdin /dev/null, output to LOG), stop it after SECS.
# Sets rc and timed_out. macOS has no timeout(1) by default.
run_limited() {
    local secs=$1 log=$2 pid ticks=0
    shift 2
    "$@" </dev/null >"$log" 2>&1 &
    pid=$!
    timed_out=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$ticks" -ge "$((secs * 5))" ]; then
            timed_out=1
            kill -TERM "$pid" 2>/dev/null
            ticks=0
            while kill -0 "$pid" 2>/dev/null && [ "$ticks" -lt 25 ]; do sleep 0.2; ticks=$((ticks + 1)); done
            kill -KILL "$pid" 2>/dev/null
            break
        fi
        sleep 0.2
        ticks=$((ticks + 1))
    done
    wait "$pid"
    rc=$?
    [ "$timed_out" = 1 ] && rc=124
    return 0
}

# wenv CMD...: CMD in this prefix with a clean Wine environment.
wenv() {
    env -u WINE_COMPATDB -u WINE_COMPATDB_FILE -u WINE_D3D_CONFIG -u WINEDLLOVERRIDES -u FEX_HARDWARE_TSO \
        WINEPREFIX="$prefix" WINEDEBUG="${LADDER_WINEDEBUG:--all}" "$@"
}

# record PHASE ARCH PROG BACKEND TSO LOG RULE SECS: classify a finished run into runs.tsv.
record() {
    local status=ok
    if [ "$timed_out" = 1 ]; then status=timeout
    elif ! grep -q "^LADDER	DONE	" "$6" 2>/dev/null; then status=crash
    elif [ "$rc" != 0 ]; then status=fail
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$status" "$rc" "$8" "logs/$(basename "$6")" "$7" >>"${tsv_out:-$runs}"
    printf '   %-44s %s (exit %s, %ss)%s\n' "$1 $2 $3${4:+ $4}${5:+ tso=$5}" "$status" "$rc" "$8" \
        "$(awk -F'\t' '$1 == "LADDER" && $2 == "DONE" { print ", " $4 }' "$6" 2>/dev/null)"
}

# A synthetic program log written by this script, for checks it makes itself (wineboot,
# shutdown, ...), so the summary parses everything the same way.
synth() {   # synth LOG STATUS CHECK DETAIL  (appends one LADDER line)
    printf 'LADDER\t%s\t%s\t%s\n' "$2" "$3" "$4" >>"$1"
}
synth_done() {   # synth_done LOG PROG
    local p f s
    p=$(grep -c "^LADDER	PASS	" "$1"); f=$(grep -c "^LADDER	FAIL	" "$1"); s=$(grep -c "^LADDER	SKIP	" "$1")
    printf 'LADDER\tDONE\t%s\tpass=%s fail=%s skip=%s info=0\n' "$2" "$p" "$f" "$s" >>"$1"
    rc=0; [ "$f" = 0 ] || rc=1
    timed_out=0
}

# run_prog PHASE ARCH PROG BACKEND TSO [RULE] [EXTRA ARGS...]
run_prog() {
    local phase=$1 arch=$2 prog=$3 backend=$4 tso=$5 rule=${6:-} exe dir log start
    shift 6 2>/dev/null || shift $#
    exe="$prog-$arch.exe"
    log="$out/logs/$phase-$arch-$prog${backend:+-$backend}${tso:+-tso$tso}.log"
    if [ ! -f "$bin/$exe" ]; then
        printf '%s\t%s\t%s\t%s\t%s\tskipped: no %s\t\t\t\t\n' "$phase" "$arch" "$prog" "$backend" "$tso" "$exe" >>"${tsv_out:-$runs}"
        return
    fi
    dir="$work/$phase-$arch-$prog${backend:+-$backend}${tso:+-$tso}"
    mkdir -p "$dir"
    cp "$bin/$exe" "$dir/"
    if [ "$backend" = dxvk ] && [ "$compatdb" = 0 ]; then   # non-bundle Wine: DXVK as native DLLs next to the exe
        local sub=x64; [ "$arch" = i686 ] && sub=x32
        cp "$LADDER_DXVK_DIR/$sub/"*.dll "$dir/" 2>/dev/null
    fi
    start=$SECONDS
    # shellcheck disable=SC2016
    run_limited "$limit" "$log" wenv sh -c 'cd "$1" && shift && exec "$@"' sh "$dir" \
        env LADDER_TIMEOUT=$((limit > 20 ? limit - 10 : limit)) ${rule:+WINE_COMPATDB=$rule} ${tso:+FEX_HARDWARE_TSO=$tso} \
        ${dll_override:+WINEDLLOVERRIDES=$dll_override} "$wine" "$exe" "$@"
    record "$phase" "$arch" "$prog" "$backend" "$tso" "$log" "$rule" $((SECONDS - start))
}

# Backends for one graphics program: "name|rule|dlloverride" lines.
backends_for() {   # backends_for PROG EXE
    local prog=$1 exe=$2 b
    if [ "$compatdb" = 1 ]; then
        case $prog in
            d3d11 | d3d12)
                for b in $backends; do [ -d "$bundle/lib/wine/dxgi/$b" ] && echo "$b|exe=$exe;dxgi=$b|"; done ;;
            d3d9)
                for b in ${LADDER_D3D9:-$(ls "$bundle/lib/wine/d3d9" 2>/dev/null)}; do
                    [ -d "$bundle/lib/wine/d3d9/$b" ] && echo "$b|exe=$exe;d3d9=$b|"
                done ;;
            opengl) for b in $gl_backends; do echo "$b|exe=$exe;opengl=$b|"; done ;;
            *) echo "default||" ;;
        esac
    else
        echo "builtin||"
        case $prog in
            d3d9 | d3d11)
                [ -n "${LADDER_DXVK_DIR:-}" ] && [ -d "$LADDER_DXVK_DIR/x64" ] && echo "dxvk||d3d9,d3d11,d3d10core,dxgi=n" ;;
        esac
    fi
}
# Backends the bundle doesn't have, for the summary.
missing_backends() {
    local b
    [ "$compatdb" = 1 ] || return 0
    for b in $backends; do [ -d "$bundle/lib/wine/dxgi/$b" ] || echo "dxgi tree $b: not in $bundle/lib/wine/dxgi"; done
}

wine_ver=$(wenv "$wine" --version 2>/dev/null | head -1)
echo "ladder: $wine (${wine_ver:-unknown version}), archs: $archs, output $out"

# ---- 1. setup -----------------------------------------------------------------------------------
phase_on() { case " $phases " in *" $1 "*) return 0 ;; esac; return 1; }
socket_dir() {   # the wineserver's directory for this prefix (/tmp/.wine-UID/server-DEV-INO)
    local dev ino
    if stat -c '%d %i' "$prefix" >/dev/null 2>&1; then read -r dev ino <<EOF
$(stat -c '%d %i' "$prefix")
EOF
    else read -r dev ino <<EOF
$(stat -f '%d %i' "$prefix")
EOF
    fi
    # Wine puts it in /tmp on Linux and in the per-user temp directory ($TMPDIR) on macOS.
    local tmp=/tmp
    [ "$(uname -s)" = Darwin ] && tmp=$(getconf DARWIN_USER_TEMP_DIR 2>/dev/null || echo "${TMPDIR:-/tmp}")
    printf '%s/.wine-%s/server-%x-%x' "${tmp%/}" "$(id -u)" "$dev" "$ino"
}

if phase_on setup; then
    echo "== setup"
    rm -rf "$prefix"; mkdir -p "$prefix"
    log=$out/logs/setup.log; : >"$log"
    start=$SECONDS
    run_limited 600 "$work/wineboot.log" wenv "$wine" wineboot -u
    cat "$work/wineboot.log" >>"$out/logs/setup-wineboot.log"
    boot_rc=$rc boot_to=$timed_out boot_s=$((SECONDS - start))
    run_limited 60 "$work/reg.log" wenv "$wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f
    [ "$(uname -s)" = Linux ] && run_limited 60 "$work/reg2.log" wenv "$wine" reg add 'HKCU\Software\Wine\X11 Driver' /v UseXRandR /t REG_SZ /d N /f
    start=$SECONDS
    run_limited 90 "$work/w.log" wenv "$wineserver" -w
    if [ "$timed_out" = 0 ]; then synth "$log" PASS prefix.server_exit "wineserver -w returned after $((SECONDS - start))s: the booted prefix leaves nothing running"
    else synth "$log" FAIL prefix.server_exit "wineserver -w still waiting after 90s: something from wineboot stayed alive"; wenv "$wineserver" -k >/dev/null 2>&1
    fi
    # The registry is saved when the server exits, so look for it only now.
    [ -s "$prefix/system.reg" ] && [ -d "$prefix/drive_c/windows/system32" ] && reg=1 || reg=0
    if [ "$boot_to" = 0 ] && [ "$boot_rc" = 0 ] && [ "$reg" = 1 ]; then
        synth "$log" PASS prefix.wineboot "wineboot -u exit 0 in ${boot_s}s; system.reg and drive_c/windows/system32 written"
    else
        synth "$log" FAIL prefix.wineboot "wineboot -u exit $boot_rc (timeout=$boot_to), registry and system32 present=$reg; see logs/setup-wineboot.log"
    fi
    synth_done "$log" prefix
    record setup - prefix "" "" "$log" "" 0
else
    [ -d "$prefix" ] || die "no prefix at $prefix and the setup phase is off"
fi

# ---- 2. serial ----------------------------------------------------------------------------------
order_args=""
[ -n "${LADDER_ORDER_ITERS:-}" ] && order_args="--iters $LADDER_ORDER_ITERS"
if phase_on serial; then
    echo "== serial"
    for arch in $archs; do
        for prog in $progs; do
            dll_override=""
            # shellcheck disable=SC2086
            if [ "$prog" = ordering ]; then run_prog serial "$arch" "$prog" "" "" "" $order_args
            else run_prog serial "$arch" "$prog" "" "" ""
            fi
        done
        for prog in $gfx; do
            backends_for "$prog" "$prog-$arch.exe" | while IFS='|' read -r b rule dll_override; do
                run_prog serial "$arch" "$prog" "$b" "" "$rule"
            done
        done
    done
    run_limited 60 "$work/w.log" wenv "$wineserver" -w
    [ "$timed_out" = 1 ] && wenv "$wineserver" -k >/dev/null 2>&1
fi

# ---- 3. concurrent ------------------------------------------------------------------------------
sentinel_arch=""
for a in $archs; do [ -f "$bin/sentinel-$a.exe" ] && { sentinel_arch=$a; break; }; done
if phase_on concurrent; then
    echo "== concurrent: $archs at once in one wineserver"
    log=$out/logs/concurrent-server.log; : >"$log"
    rm -f "$prefix/drive_c/ladder-stop"
    if [ -n "$sentinel_arch" ]; then
        cp "$bin/sentinel-$sentinel_arch.exe" "$work/"
        (cd "$work" && wenv "$wine" "sentinel-$sentinel_arch.exe" "C:\\ladder-stop" 3600 >"$out/logs/concurrent-sentinel.log" 2>&1) &
        sentinel_pid=$!
        sleep 3
    fi
    pids=""
    for arch in $archs; do
        (
            tsv_out=$work/concurrent-$arch.tsv
            dll_override=""
            for prog in $progs $gfx; do run_prog concurrent "$arch" "$prog" "" "" ""; done
        ) >"$work/concurrent-$arch.out" 2>&1 &
        pids="$pids $!"
    done
    # shellcheck disable=SC2086
    wait $pids
    for arch in $archs; do cat "$work/concurrent-$arch.out"; cat "$work/concurrent-$arch.tsv" >>"$runs" 2>/dev/null; done
    if [ -n "$sentinel_arch" ]; then
        if kill -0 "$sentinel_pid" 2>/dev/null; then
            synth "$log" PASS server.one_lifetime "the sentinel ($sentinel_arch) held one wineserver for every concurrent run"
        else
            synth "$log" FAIL server.one_lifetime "the sentinel exited during the concurrent phase; see logs/concurrent-sentinel.log"
        fi
        : >"$prefix/drive_c/ladder-stop"
        n=0; while kill -0 "$sentinel_pid" 2>/dev/null && [ $n -lt 150 ]; do sleep 0.2; n=$((n + 1)); done
        if grep -q "^LADDER	PASS	sentinel.stop" "$out/logs/concurrent-sentinel.log"; then
            synth "$log" PASS server.sentinel_stop "sentinel saw C:\\ladder-stop and exited"
        else
            synth "$log" FAIL server.sentinel_stop "sentinel did not stop cleanly"
            kill "$sentinel_pid" 2>/dev/null
        fi
        rm -f "$prefix/drive_c/ladder-stop"
    else
        synth "$log" SKIP server.one_lifetime "no sentinel-<arch>.exe built"
    fi
    synth_done "$log" server
    record concurrent - server "" "" "$log" "" 0
fi

# ---- 4. ordering with FEX_HARDWARE_TSO=1 and =0 ---------------------------------------------------
if phase_on tso; then
    if [ "$host_arm" = 1 ]; then
        echo "== tso: ordering suite for x86 builds with hardware TSO on and off"
        for tso in 1 0; do
            for arch in x86_64 i686; do
                case " $archs " in *" $arch "*) ;; *) continue ;; esac
                dll_override=""
                # shellcheck disable=SC2086
                run_prog tso "$arch" ordering "" "$tso" "" $order_args
            done
        done
    else
        echo "== tso: skipped (x86 host: no FEX, FEX_HARDWARE_TSO has no effect)"
        printf 'tso\t-\tordering\t\t\tskipped: x86 host, no FEX\t\t\t\t\n' >>"$runs"
    fi
fi

# ---- 5. heavy, optional ---------------------------------------------------------------------------
if phase_on heavy && [ -n "$heavy" ]; then
    echo "== heavy: $heavy"
    for h in $heavy; do
        "$here/heavy.sh" "$h" "$wine" "$prefix" "$out" "$work" "$compatdb" "$limit" >>"$runs" || true
    done
fi

# ---- 6. shutdown ----------------------------------------------------------------------------------
if phase_on shutdown; then
    echo "== shutdown"
    log=$out/logs/shutdown.log; : >"$log"
    sock=$(socket_dir)
    start=$SECONDS
    run_limited 60 "$work/w.log" wenv "$wineserver" -w
    if [ "$timed_out" = 0 ]; then
        synth "$log" PASS shutdown.graceful "wineserver -w returned after $((SECONDS - start))s: no process outlived the ladder"
    else
        procs=$(ps ax -o pid=,command= 2>/dev/null | grep -E '\.exe' | grep -v grep | head -10 | tr '\n' ';')
        synth "$log" FAIL shutdown.graceful "wineserver still serving after 60s; .exe processes: ${procs:-none visible}"
    fi
    run_limited 30 "$work/k.log" wenv "$wineserver" -k
    n=0; while [ -e "$sock/socket" ] && [ $n -lt 50 ]; do sleep 0.2; n=$((n + 1)); done
    left=$(ps ax -o command= 2>/dev/null | grep -E '(threads|seh|callbacks|memory|ordering|d3d9|d3d11|d3d12|opengl|vulkan|sentinel)-(aarch64|arm64ec|x86_64|i686)\.exe' | grep -vc grep)
    if [ ! -e "$sock/socket" ] && [ "$left" = 0 ]; then
        synth "$log" PASS shutdown.kill "wineserver -k: server socket gone, no ladder process left"
    else
        synth "$log" FAIL shutdown.kill "after wineserver -k: socket $( [ -e "$sock/socket" ] && echo "still at $sock" || echo gone), $left ladder processes left"
    fi
    synth_done "$log" shutdown
    record shutdown - shutdown "" "" "$log" "" 0
fi

# ---- summary ----------------------------------------------------------------------------------------
{
    echo "wine	$wine"
    echo "wine_version	${wine_ver:-unknown}"
    echo "bundle	${bundle:-system}"
    echo "compatdb	$compatdb"
    echo "host	$(uname -sm)$(command -v sw_vers >/dev/null && echo ", macOS $(sw_vers -productVersion)")"
    echo "archs	$archs"
    echo "date	$date"
    missing_backends | sed 's/^/note	/'
    [ "$host_arm" = 1 ] || echo "note	x86 host: aarch64/arm64ec builds and the FEX_HARDWARE_TSO phase need the Mac"
} >"$out/env.tsv"
"$python" "$here/summarize.py" "$out" --known "$here/known-failures.txt"
status=$?
echo "ladder: wrote $out/summary.md"
exit $status
