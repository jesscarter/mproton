#!/bin/bash
# Optional real-world rungs for tests/ladder/run.sh (LADDER_HEAVY="mono chromium").
# Called by run.sh; prints runs.tsv rows on stdout and writes logs to OUT/logs/heavy-*.log.
#
# Usage: heavy.sh RUNG WINE PREFIX OUT WORK COMPATDB TIMEOUT
#
# mono      Builds src/heavy/hello.cs as AnyCPU and x86 with the first of: mcs, csc, or
#           LADDER_MONO_EXE (a prebuilt AnyCPU exe), then runs it on Wine Mono. Wine Mono must
#           be in the prefix: set LADDER_WINE_MONO_MSI=<wine-mono-*-x86.msi> to have it
#           installed first (https://dl.winehq.org/wine/wine-mono/). mach9 bundles turn mscoree
#           off by default (compatdb dll_overrides=mscoree,mshtml=), so this rung turns it back
#           on for its own exe with a compatdb rule and WINEDLLOVERRIDES.
# chromium  Runs LADDER_CHROMIUM=<chrome.exe> headless with --dump-dom on a local page whose
#           JavaScript computes a checksum (V8's JIT under FEX), and checks the printed DOM. The
#           browser starts renderer, GPU and utility subprocesses, the same process model as
#           the Steam client's steamwebhelper.exe (CEF), which is the real target but needs a
#           Steam login and is not automated. Runs once with --no-sandbox (must pass) and once
#           with Chromium's sandbox (reported as INFO: the sandbox under Wine is its own topic).
set -u
rung=$1 wine=$2 prefix=$3 out=$4 work=$5 compatdb=$6 limit=$7
here=$(cd "$(dirname "$0")" && pwd)

row() {   # row PROG STATUS EXIT SECS LOG [RULE]
    printf 'heavy\t-\t%s\t\t\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "${5:+logs/$(basename "$5")}" "${6:-}"
}
skip_row() { printf 'heavy\t-\t%s\t\t\tskipped: %s\t\t\t\t\n' "$1" "$2"; echo "   heavy $1: skipped ($2)" >&2; }

run_limited() {   # same as run.sh's
    local secs=$1 log=$2 pid ticks=0
    shift 2
    "$@" </dev/null >"$log" 2>&1 &
    pid=$!
    timed_out=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$ticks" -ge "$((secs * 5))" ]; then
            timed_out=1; kill -TERM "$pid" 2>/dev/null; sleep 2; kill -KILL "$pid" 2>/dev/null; break
        fi
        sleep 0.2; ticks=$((ticks + 1))
    done
    wait "$pid"; rc=$?
    [ "$timed_out" = 1 ] && rc=124
    return 0
}
status_of() {   # status_of LOG -> ok/fail/crash/timeout
    if [ "$timed_out" = 1 ]; then echo timeout
    elif ! grep -q "^LADDER	DONE	" "$1"; then echo crash
    elif [ "$rc" != 0 ]; then echo fail
    else echo ok
    fi
}
wenv() {
    env -u WINE_COMPATDB -u WINE_COMPATDB_FILE -u WINEDLLOVERRIDES WINEPREFIX="$prefix" WINEDEBUG="${LADDER_WINEDEBUG:--all}" "$@"
}

case $rung in
mono)
    src=$here/src/heavy/hello.cs
    exes=""
    if [ -n "${LADDER_MONO_EXE:-}" ]; then
        cp "$LADDER_MONO_EXE" "$work/mono-anycpu.exe" && exes="mono-anycpu.exe"
    else
        cc=""
        for c in mcs csc; do command -v "$c" >/dev/null && { cc=$c; break; }; done
        [ -n "$cc" ] || { skip_row mono "no C# compiler (mcs/csc) and no LADDER_MONO_EXE"; exit 0; }
        for plat in anycpu x86; do
            if "$cc" -nologo -platform:$plat -out:"$work/mono-$plat.exe" "$src" >"$work/mcs-$plat.log" 2>&1; then
                exes="$exes mono-$plat.exe"
            else
                skip_row "mono-$plat" "$cc failed: $(head -c 200 "$work/mcs-$plat.log" | tr '\n\t' '  ')"
            fi
        done
    fi
    if [ -n "${LADDER_WINE_MONO_MSI:-}" ]; then
        run_limited 600 "$out/logs/heavy-mono-install.log" wenv "$wine" msiexec /i "$LADDER_WINE_MONO_MSI" /qn
    fi
    if ! ls -d "$prefix/drive_c/windows/mono/mono-2.0" "$prefix/drive_c/windows/mono"/* >/dev/null 2>&1; then
        skip_row mono "Wine Mono is not installed in the prefix (set LADDER_WINE_MONO_MSI)"
        exit 0
    fi
    for exe in $exes; do
        log=$out/logs/heavy-${exe%.exe}.log
        rule=""
        [ "$compatdb" = 1 ] && rule="exe=$exe;dll_overrides=mscoree=b"
        start=$SECONDS
        # shellcheck disable=SC2016
        run_limited "$limit" "$log" wenv sh -c 'cd "$1" && shift && exec "$@"' sh "$work" \
            env WINEDLLOVERRIDES=mscoree=b ${rule:+WINE_COMPATDB=$rule} "$wine" "$exe"
        st=$(status_of "$log")
        row "${exe%.exe}" "$st" "$rc" $((SECONDS - start)) "$log"
        echo "   heavy ${exe%.exe}: $st (exit $rc)" >&2
    done
    ;;
chromium)
    chrome=${LADDER_CHROMIUM:-}
    [ -n "$chrome" ] && [ -f "$chrome" ] || { skip_row chromium "LADDER_CHROMIUM not set to a Windows chrome.exe"; exit 0; }
    # The page: JIT-heavy loop, sort, regex and JSON; the result goes into the DOM.
    cat >"$prefix/drive_c/ladder-chromium.html" <<'EOF'
<!doctype html><html><body><p id="r">pending</p><script>
let s = 0;
for (let i = 0; i < 3000000; i++) s = (s * 31 + (i * i) % 1000003) % 1000000007;
const a = []; for (let i = 0; i < 100000; i++) a.push((i * 7919) % 100003);
a.sort((x, y) => x - y);
const j = JSON.parse(JSON.stringify({n: a.length, last: a[a.length - 1]}));
const m = "ladder-42-ok".match(/(\d+)/)[1];
document.getElementById("r").textContent = "LADDER-CHROMIUM-OK " + s + " " + j.n + " " + j.last + " " + m;
</script></body></html>
EOF
    want=$(python3 -c '
s = 0
for i in range(3000000): s = (s * 31 + (i * i) % 1000003) % 1000000007
a = sorted((i * 7919) % 100003 for i in range(100000))
print("LADDER-CHROMIUM-OK %d %d %d 42" % (s, len(a), a[-1]))')
    cdir=$(cd "$(dirname "$chrome")" && pwd)
    for variant in nosandbox sandbox; do
        log=$out/logs/heavy-chromium-$variant.log
        flags="--headless=new --disable-gpu --no-first-run --no-default-browser-check --user-data-dir=C:\\ladder-chrome-$variant"
        [ "$variant" = nosandbox ] && flags="$flags --no-sandbox"
        start=$SECONDS
        # shellcheck disable=SC2016,SC2086
        run_limited "$limit" "$work/chrome-$variant.out" wenv sh -c 'cd "$1" && shift && exec "$@"' sh "$cdir" \
            "$wine" "$(basename "$chrome")" $flags --dump-dom "file:///C:/ladder-chromium.html"
        secs=$((SECONDS - start))
        cp "$work/chrome-$variant.out" "$log"
        status=FAIL
        grep -qF "$want" "$log" && status=PASS
        [ "$variant" = sandbox ] && [ "$status" = FAIL ] && status=INFO
        printf 'LADDER\t%s\tchromium.dump_dom_%s\t%s\n' "$status" "$variant" \
            "$( [ "$status" = PASS ] && echo "page JS result printed by the headless browser in ${secs}s (exit $rc)" || echo "expected '$want' not in the output (exit $rc, timeout=$timed_out)")" >>"$log"
        sleep 2
        left=$(ps ax -o command= 2>/dev/null | grep -i 'chrome\.exe' | grep -vc grep)
        if [ "$left" = 0 ]; then printf 'LADDER\tPASS\tchromium.exit_%s\tno chrome.exe process left after the browser exited\n' "$variant" >>"$log"
        else printf 'LADDER\tFAIL\tchromium.exit_%s\t%s chrome.exe processes still running\n' "$variant" "$left" >>"$log"
            pkill -f 'chrome\.exe' 2>/dev/null
        fi
        f=$(grep -c "^LADDER	FAIL	" "$log")
        printf 'LADDER\tDONE\tchromium\tpass=%s fail=%s skip=0 info=0\n' "$(grep -c "^LADDER	PASS	" "$log")" "$f" >>"$log"
        st=ok; [ "$f" = 0 ] || st=fail
        row "chromium-$variant" "$st" "$rc" "$secs" "$log"
        echo "   heavy chromium-$variant: $st (exit $rc, ${secs}s)" >&2
    done
    ;;
*)
    skip_row "$rung" "unknown heavy rung (known: mono chromium)"
    ;;
esac
