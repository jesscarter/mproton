#!/bin/bash
# Run mpbench across every runtime / arch / D3D backend we can test on this Mac and write
# results/m1-<date>/: one mpbench JSON per run, logs/, matrix.tsv and summary.md.
#
# Usage: tools/run-matrix.sh BUNDLE_DIR ROSETTA_WINE
#   BUNDLE_DIR    the ARM64 Wine bundle from athei/mach9's bundle-wine.sh (has bin/wine and
#                 lib/wine/dxgi/<impl>/ trees), or "-" to skip the ARM64 runtimes
#   ROSETTA_WINE  "/Applications/Wine Stable.app", its bin/ directory or its bin/wine,
#                 or "-" to skip the Rosetta runtime
#
# Runtimes:
#   rosetta     Rosetta Wine 10 (x86_64 build under Rosetta 2), wined3d. x86_64 and i686 only.
#   arm64-tso1  ARM64 Wine + FEX, FEX_HARDWARE_TSO=1 (hardware TSO). All four mpbench builds.
#   arm64-tso0  ARM64 Wine + FEX, FEX_HARDWARE_TSO=0 (FEX emulates ordering). x86_64 and i686
#               only: aarch64/arm64ec mpbench is native code, which FEX's TSO mode doesn't touch.
# Parts per runtime and arch:
#   cpu         CPU scenes only (--no-gpu); backend-independent, so run once
#   <backend>   GPU scenes only (--no-cpu --api all), with the dxgi tree chosen by a compatdb rule
#               (WINE_COMPATDB="exe=mpbench-<arch>.exe;dxgi=<backend>"):
#                 wined3d  D3D11 on wined3d; D3D12 runs on whatever the wined3d tree provides
#                 dxmt     D3D11 on DXMT, D3D12 on vkd3d-proton (the dxmt tree carries both)
#                 dxvk     only if BUNDLE_DIR/lib/wine/dxgi/dxvk/ exists
#               The compatdb block mpbench's process logs is checked, and a run that got another
#               tree is flagged as a backend mismatch. Rosetta has no compatdb: wined3d only.
#
# Every runtime gets a fresh prefix. Every run has a timeout; a crash, a timeout or missing JSON
# is recorded (a stub JSON with a "run" object) and the matrix carries on. Scenes mpbench skips
# because their API's DLL is missing on that tree (e.g. no d3d12.dll in wined3d) are not
# failures: summary.md lists them under "Not available on this backend" and the tables show
# "n/a: <reason>".
#
# Environment:
#   MATRIX_RUNTIMES  default "rosetta arm64-tso1 arm64-tso0"
#   MATRIX_ARCHS     default "x86_64 i686 aarch64 arm64ec"
#   MATRIX_BACKENDS  default "wined3d dxmt dxvk"
#   MATRIX_TIMEOUT   seconds per mpbench run (default 900)
#   MATRIX_FRAMES    frames per GPU scene (default 600, 1280x720 like the M0 baseline)
#   MATRIX_DATE      default today (YYYY-MM-DD); output goes to results/m1-<date>/
#   MATRIX_OUT       override the output directory
#   MATRIX_PREFIXES  where the prefixes go (default prefixes/m1-<date>/, gitignored)
#   MATRIX_ARGS      extra mpbench arguments for every run
#   MPBENCH_DIR      the mpbench builds (default bench/out, from bench/build.sh)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
[ $# -eq 2 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
bundle_arg=$1 rosetta_arg=$2

date=${MATRIX_DATE:-$(date +%F)}
out=${MATRIX_OUT:-$root/results/m1-$date}
prefixes=${MATRIX_PREFIXES:-$root/prefixes/m1-$date}
mpdir=${MPBENCH_DIR:-$root/bench/out}
runtimes=${MATRIX_RUNTIMES:-rosetta arm64-tso1 arm64-tso0}
archs=${MATRIX_ARCHS:-x86_64 i686 aarch64 arm64ec}
backends=${MATRIX_BACKENDS:-wined3d dxmt dxvk}
limit=${MATRIX_TIMEOUT:-900}
frames=${MATRIX_FRAMES:-600}
extra=${MATRIX_ARGS:-}
python=${PYTHON:-python3}

die() { echo "run-matrix: $*" >&2; exit 2; }

# ---- inputs ---------------------------------------------------------------------------------
bundle="" rosetta_wine=""
if [ "$bundle_arg" != - ]; then
    bundle=$(cd "$bundle_arg" 2>/dev/null && pwd) || die "no bundle directory $bundle_arg"
    [ -x "$bundle/bin/wine" ] || die "$bundle has no bin/wine"
fi
if [ "$rosetta_arg" != - ]; then
    for c in "$rosetta_arg/Contents/Resources/wine/bin/wine" "$rosetta_arg/bin/wine" "$rosetta_arg/wine" "$rosetta_arg"; do
        if [ -f "$c" ] && [ -x "$c" ]; then rosetta_wine=$c; break; fi
    done
    [ -n "$rosetta_wine" ] || die "no wine binary at $rosetta_arg"
fi
have_exe=0
for a in $archs; do [ -f "$mpdir/mpbench-$a.exe" ] && have_exe=1; done
[ "$have_exe" = 1 ] || die "no mpbench builds in $mpdir (run bench/build.sh)"

mkdir -p "$out/logs" "$prefixes"
work=$(mktemp -d "${TMPDIR:-/tmp}/run-matrix.XXXXXX")
trap 'rm -rf "$work"' EXIT
tsv="$out/matrix.tsv"
printf 'file\truntime\tarch\tpart\tstatus\texit\tseconds\tresults\tordering_errors\tbackend\tnot_available\n' >"$tsv"

# run_limited SECS LOG CMD...: run CMD (stdin from /dev/null, output to LOG), stop it after
# SECS. Sets rc and timed_out. macOS has no timeout(1) by default.
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
}

# Per-runtime state, set by use_runtime.
wine="" wineserver="" tso="" rt_archs="" rt_backends="" rt_desc=""
use_runtime() {
    case $1 in
        rosetta)
            [ -n "$rosetta_wine" ] || return 1
            wine=$rosetta_wine tso="" rt_archs="x86_64 i686" rt_backends=wined3d
            rt_desc="Rosetta Wine ($rosetta_wine)" ;;
        arm64-tso1 | arm64-tso0)
            [ -n "$bundle" ] || return 1
            wine=$bundle/bin/wine tso=${1#arm64-tso}
            rt_archs="x86_64 i686 aarch64 arm64ec"
            [ "$tso" = 0 ] && rt_archs="x86_64 i686"
            rt_backends=$backends
            rt_desc="ARM64 Wine + FEX, FEX_HARDWARE_TSO=$tso ($bundle)" ;;
        *) die "unknown runtime $1" ;;
    esac
    wineserver=$(dirname "$wine")/wineserver
    [ -x "$wineserver" ] || wineserver=""
}

# wenv CMD...: run CMD with this runtime's Wine environment.
wenv() {
    env -u WINE_COMPATDB -u WINE_COMPATDB_FILE -u WINE_D3D_CONFIG -u WINEDLLOVERRIDES -u FEX_HARDWARE_TSO \
        WINEPREFIX="$prefix" WINEDEBUG=-all ${tso:+FEX_HARDWARE_TSO=$tso} "$@"
}

stop_wineserver() {   # stop_wineserver kill|wait
    [ -n "$wineserver" ] || return 0
    if [ "$1" = kill ]; then
        wenv "$wineserver" -k >/dev/null 2>&1
    else
        run_limited 60 "$work/wineserver.log" wenv "$wineserver" -w
        [ "$timed_out" = 1 ] && wenv "$wineserver" -k >/dev/null 2>&1
    fi
    return 0
}

skip() {   # skip RUNTIME ARCH PART REASON
    printf '%s\t%s\t%s\t%s\tskipped: %s\t\t\t\t\t\t\n' "-" "$1" "$2" "$3" "$4" >>"$tsv"
    echo "   skip $1 $2 $3: $4"
}

# ---- the matrix -----------------------------------------------------------------------------
echo "run-matrix: output $out"
versions=""
for rt in $runtimes; do
    if ! use_runtime "$rt"; then
        echo "== $rt: skipped (no $( [ "$rt" = rosetta ] && echo "Rosetta Wine" || echo bundle) given)"
        skip "$rt" all all "runtime not given"
        continue
    fi
    echo "== $rt: $rt_desc"
    prefix="$prefixes/$rt"
    rm -rf "$prefix"
    mkdir -p "$prefix"
    run_limited 30 "$work/version" wenv "$wine" --version
    ver=$(head -1 "$work/version")
    versions="$versions- **$rt**: $rt_desc, \`${ver:-unknown version}\`
"
    echo "   $ver; creating a fresh prefix"
    run_limited 600 "$out/logs/$rt-wineboot.log" wenv "$wine" wineboot -u
    [ "$rc" = 0 ] || echo "   WARNING: wineboot exited $rc (log: logs/$rt-wineboot.log); carrying on"
    run_limited 60 "$work/reg.log" wenv "$wine" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f
    stop_wineserver wait

    for arch in $archs; do
        case " $rt_archs " in *" $arch "*) ;; *) skip "$rt" "$arch" all "$arch not run on $rt"; continue ;; esac
        exe="mpbench-$arch.exe"
        [ -f "$mpdir/$exe" ] || { skip "$rt" "$arch" all "no $mpdir/$exe"; continue; }
        for part in cpu $rt_backends; do
            name="$rt-$arch-$part"
            rule="" want=""
            if [ "$part" = cpu ]; then
                args="--no-gpu"
            else
                args="--no-cpu --api all --frames $frames --width 1280 --height 720"
                if [ "$rt" != rosetta ]; then
                    if [ ! -d "$bundle/lib/wine/dxgi/$part" ]; then
                        skip "$rt" "$arch" "$part" "no lib/wine/dxgi/$part tree in the bundle"
                        continue
                    fi
                    rule="exe=$exe;dxgi=$part" want=$part
                fi
            fi
            note="m1 matrix: $rt_desc; ${ver:-?}; part $part${rule:+; WINE_COMPATDB=$rule}"
            dir="$work/runs/$name"
            mkdir -p "$dir"
            cp "$mpdir/$exe" "$dir/"
            log="$out/logs/$name.log"
            printf '   %-28s ' "$name"
            start=$SECONDS
            # $args and $extra are word lists on purpose; the sh -c script expands in sh.
            # shellcheck disable=SC2086,SC2016
            run_limited "$limit" "$log" wenv sh -c 'cd "$1" && shift && exec "$@"' sh "$dir" \
                env ${rule:+WINE_COMPATDB=$rule} "$wine" "$exe" $args $extra --note "$note" --out result.json
            secs=$((SECONDS - start)) run_rc=$rc
            if [ "$timed_out" = 1 ]; then status=timeout; stop_wineserver kill
            elif [ "$run_rc" = 0 ]; then status=ok; stop_wineserver wait
            else status=crash; stop_wineserver wait
            fi
            json="$out/$name.json"
            rm -f "$json"
            [ -f "$dir/result.json" ] && mv "$dir/result.json" "$json"
            line=$("$python" "$root/tools/matrix_record.py" --json "$json" --status "$status" --exit "$run_rc" \
                --seconds "$secs" --log "$log" --runtime "$rt" --arch "$arch" --part "$part" \
                --note "$note" ${want:+--want-dxgi "$want"})
            echo "$line" >>"$tsv"
            echo "$line" | awk -F'\t' '{ printf "%s (exit %s, %ss, %s results%s%s%s)\n", $5, $6, $7, $8,
                ($9 != "" && $9 != "0") ? ", " $9 " ORDERING ERRORS" : "", ($10 != "") ? ", backend " $10 : "",
                ($11 != "") ? ", not available: " $11 : "" }'
        done
    done
    stop_wineserver kill
done

# ---- summary --------------------------------------------------------------------------------
label() {   # label FILE-STEM -> column label (no commas: compare.py splits --labels on them)
    local rt=${1%%-*} rest=${1#*-} part
    case $1 in arm64-tso1-*) rt="TSO=1"; rest=${1#arm64-tso1-} ;; arm64-tso0-*) rt="TSO=0"; rest=${1#arm64-tso0-} ;; esac
    part=${rest#*-}
    [ "$part" = dxmt ] && part="dxmt + vkd3d-proton"
    [ "$part" = cpu ] && part=""
    echo "$rt${part:+ $part}"
}

compare() {   # compare TITLE FILE... : a highlighted table, or nothing if no files
    local title=$1 labels="" f
    shift
    [ $# -gt 0 ] || return 0
    for f in "$@"; do labels="$labels${labels:+,}$(label "${f%.json}")"; done
    printf '### %s\n\n' "$title"
    (cd "$out" && "$python" "$root/bench/compare.py" --highlight --labels "$labels" "$@") \
        || echo "(compare.py failed on: $*)"
    echo
}

{
    echo "# M1 matrix, $date"
    echo
    echo "Generated by \`tools/run-matrix.sh\` on $(uname -sm)$(command -v sw_vers >/dev/null && echo ", macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))")."
    echo "GPU scenes: $frames frames at 1280x720; timeout ${limit}s per run. Logs are in \`logs/\`, every run in \`matrix.tsv\`."
    echo
    printf '%s' "$versions"
    echo
    echo "## Problems"
    echo
    problems=$(awk -F'\t' 'NR > 1 && $5 !~ /^skipped/ {
            if ($5 != "ok") printf "- **%s**: %s (exit %s after %ss)\n", $1, $5, $6, $7
            if ($9 != "" && $9 != "0") printf "- **%s**: cpu.pingpong %s ordering errors\n", $1, $9
            if ($10 == "mismatch") printf "- **%s**: compatdb gave mpbench a different dxgi tree than the rule asked for (see the run row)\n", $1
            if ($10 == "unchecked") printf "- %s: no compatdb block for mpbench in the log, backend not verified\n", $1
        }' "$tsv")
    if [ -n "$problems" ]; then echo "$problems"; else echo "None at the run level. Scene-level flags are under each table."; fi
    skipped=$(awk -F'\t' 'NR > 1 && $5 ~ /^skipped/ { printf "- %s %s %s: %s\n", $2, $3, $4, substr($5, 10) }' "$tsv")
    if [ -n "$skipped" ]; then echo; echo "Not run:"; echo; echo "$skipped"; fi
    na=$(awk -F'\t' 'NR > 1 && $11 != "" { printf "- %s: %s\n", $1, $11 }' "$tsv")
    if [ -n "$na" ]; then
        echo
        echo "## Not available on this backend"
        echo
        echo "mpbench skipped these APIs because a DLL or entry point is missing on that tree. Not failures; the tables show them as n/a."
        echo
        echo "$na"
    fi
    echo
    for arch in $archs; do
        cpu_files="" gpu_files=""
        for rt in $runtimes; do
            [ -f "$out/$rt-$arch-cpu.json" ] && cpu_files="$cpu_files $rt-$arch-cpu.json"
            for b in wined3d dxmt dxvk $backends; do
                f="$rt-$arch-$b.json"
                [ -f "$out/$f" ] || continue
                case " $gpu_files " in *" $f "*) ;; *) gpu_files="$gpu_files $f" ;; esac
            done
        done
        [ -n "$cpu_files$gpu_files" ] || continue
        echo "## $arch"
        echo
        # shellcheck disable=SC2086  # space-separated file lists without spaces in the names
        compare "$arch CPU" $cpu_files
        # shellcheck disable=SC2086
        compare "$arch GPU" $gpu_files
    done
} >"$out/summary.md"

echo "run-matrix: wrote $out/summary.md"
grep -c . "$tsv" | awk '{ print "run-matrix: " $1 - 1 " rows in matrix.tsv" }'
