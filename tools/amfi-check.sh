#!/bin/bash
# After rebooting with amfi_get_out_of_my_way=1 (and Permissive Security), check whether an
# ad-hoc-signed binary may now claim com.apple.developer.cross-architecture-support, and
# whether per-thread TSO and the 4K-page spawn work with it.
#
# Usage: tools/amfi-check.sh            (no sudo needed; prints the one command that does)
#
# Steps, each reported on its own:
#   1. csrutil status, nvram boot-args, the Startup Security policy (bputil needs sudo)
#   2. build probes/macos_compat_probe.c twice: a plain arm64 binary, and a child linked with
#      -Wl,-x86_64_layout_emulation (macOS 26.4+ linkers; skipped if the linker refuses)
#   3. control run: same probe signed ad hoc WITHOUT the entitlement (expect KERN_FAILURE/EBADMACHO)
#   4. entitled run: signed ad hoc WITH the entitlement; exit code + newest
#      ~/Library/Logs/DiagnosticReports/probe*.ips checked for a CODESIGNING kill
#   5. TSO: thread_set_x86_64_compat result and the message-passing litmus
#   6. 4K spawn: posix_spawn with the 4K attribute, once per child binary, child's page size
# Last line: "VERDICT: PASS|PARTIAL|FAIL ...". Exit status 0 for PASS, 1 for PARTIAL, 2 for FAIL.
#
# Overrides (mainly for tests): AMFI_CHECK_WORK (build dir, default a temp dir),
# AMFI_DIAG_DIR (crash reports, default ~/Library/Logs/DiagnosticReports),
# AMFI_TIMEOUT (seconds per probe run, default 60), AMFI_MP_ITERS (litmus iterations).
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/probes/macos_compat_probe.c"
work=${AMFI_CHECK_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/amfi-check.XXXXXX")}
diag=${AMFI_DIAG_DIR:-$HOME/Library/Logs/DiagnosticReports}
limit=${AMFI_TIMEOUT:-60}
mp_iters=${AMFI_MP_ITERS:-20000000}
ent_key=com.apple.developer.cross-architecture-support
mkdir -p "$work"

hr() { printf '\n== %s\n' "$*"; }
# value KEY FILE: the value of the last "RESULT KEY=value" line in FILE
value() { sed -n "s/^RESULT $1=//p" "$2" | tail -1; }

# run_limited SECS OUTFILE CMD...: run CMD with output to OUTFILE, killing it after SECS.
# Sets rc (124 on timeout). macOS has no timeout(1) by default.
run_limited() {
    local secs=$1 out=$2 pid n=0
    shift 2
    "$@" >"$out" 2>&1 &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$n" -ge "$((secs * 10))" ]; then
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            rc=124
            return
        fi
        sleep 0.1
        n=$((n + 1))
    done
    wait "$pid"
    rc=$?
}

# ---- 1. system state -----------------------------------------------------------------------
hr "System"
sw_vers 2>/dev/null | sed 's/^/  /'
echo "  arch: $(uname -m)"

hr "SIP"
csr=$(csrutil status 2>&1)
echo "  $csr"
sip_off=0
case $csr in *disabled*) sip_off=1 ;; esac

hr "Boot args"
bootargs=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//')
echo "  boot-args: ${bootargs:-(none set)}"
amfi_arg=0
case " $bootargs " in *" amfi_get_out_of_my_way=1 "* | *" amfi_get_out_of_my_way=0x1 "*) amfi_arg=1 ;; esac
[ "$amfi_arg" = 1 ] && echo "  amfi_get_out_of_my_way=1 is set" || echo "  amfi_get_out_of_my_way=1 is NOT set"

hr "Startup Security policy"
if [ "$(id -u)" = 0 ] && command -v bputil >/dev/null; then
    bputil -d 2>&1 | sed 's/^/  /'
else
    echo "  bputil needs root, so it isn't run here. To see the policy, run:"
    echo "      sudo bputil -d"
    echo "  Look for 'Security Mode: Permissive' (Permissive Security) and"
    echo "  'Boot Args Filtering Status: Disabled' (boot-args, including amfi_get_out_of_my_way, honoured)."
fi

# ---- 2. build -------------------------------------------------------------------------------
hr "Build ($work)"
probe="$work/probe"
child="$work/probe-x86layout"
noent="$work/probe-noent"
ents="$work/entitlements.plist"
cat >"$ents" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>$ent_key</key><true/></dict></plist>
EOF
if ! clang -O2 -arch arm64 -o "$probe" "$src" >"$work/build.log" 2>&1; then
    cat "$work/build.log"
    echo "VERDICT: FAIL - the probe did not build (see above)"
    exit 2
fi
echo "  built probe (arm64)"
layout=0
if clang -O2 -arch arm64 -Wl,-x86_64_layout_emulation -o "$child" "$src" >"$work/build-layout.log" 2>&1; then
    layout=1
    echo "  built probe-x86layout (-Wl,-x86_64_layout_emulation)"
else
    echo "  probe-x86layout NOT built: the linker rejected -Wl,-x86_64_layout_emulation (needs macOS 26.4+ ld):"
    sed 's/^/      /' "$work/build-layout.log" | head -5
fi
cp "$probe" "$noent"
sign_ok=1
codesign -f -s - "$noent" >"$work/sign.log" 2>&1 || sign_ok=0
codesign -f -s - --entitlements "$ents" "$probe" >>"$work/sign.log" 2>&1 || sign_ok=0
[ "$layout" = 1 ] && { codesign -f -s - --entitlements "$ents" "$child" >>"$work/sign.log" 2>&1 || sign_ok=0; }
if [ "$sign_ok" = 0 ]; then
    cat "$work/sign.log"
    echo "VERDICT: FAIL - codesign failed (see above)"
    exit 2
fi
if codesign -d --entitlements - --xml "$probe" 2>/dev/null | grep -q "$ent_key"; then
    echo "  signed ad hoc; probe claims $ent_key"
else
    echo "  WARNING: the signed probe does not show $ent_key in its entitlements"
fi
children=(--spawn-child "$probe")
[ "$layout" = 1 ] && children+=(--spawn-child "$child")

# ---- 3. control run -------------------------------------------------------------------------
hr "Control: ad hoc, no entitlement"
run_limited "$limit" "$work/noent.out" "$noent" --mp-iters 0
echo "  exit $rc"
echo "  thread_set_x86_64_compat: $(value tso_set "$work/noent.out") (5 = KERN_FAILURE, expected without the entitlement)"
echo "  4K spawn: posix_spawn -> $(value spawn4k.probe-noent.rc "$work/noent.out") (88 = EBADMACHO, expected without the entitlement)"

# ---- 4. entitled run ------------------------------------------------------------------------
hr "Launch: ad hoc, claiming $ent_key"
marker="$work/.start"
: >"$marker"
sleep 1    # crash reports are compared by mtime with this marker
run_limited "$limit" "$work/probe.out" "$probe" --mp-iters "$mp_iters" "${children[@]}"
launch_rc=$rc
echo "  exit $launch_rc$([ "$launch_rc" -gt 128 ] && [ "$launch_rc" != 124 ] && echo " (signal $((launch_rc - 128)))")"
[ "$launch_rc" = 124 ] && echo "  TIMED OUT after ${limit}s"
sleep 2    # ReportCrash writes the .ips a moment after the kill
report=""
if [ -d "$diag" ]; then
    for f in "$diag"/probe*.ips; do
        [ -f "$f" ] && [ "$f" -nt "$marker" ] || continue
        if [ -z "$report" ] || [ "$f" -nt "$report" ]; then report=$f; fi
    done
fi
codesigning_kill=0
if [ -n "$report" ]; then
    echo "  crash report: $report"
    grep -o '"signal" *: *"[^"]*"\|"namespace" *: *"[^"]*"\|"indicator" *: *"[^"]*"' "$report" | sort -u | sed 's/^/    /'
    grep -q 'CODESIGNING\|Code Signature Invalid\|Taskgated Invalid Signature' "$report" && codesigning_kill=1
else
    echo "  no new probe*.ips in $diag"
fi
launched=0
[ "$(value launched "$work/probe.out")" = 1 ] && launched=1
if [ "$launched" = 1 ]; then
    echo "  the probe started and ran (exit $launch_rc)"
elif [ "$codesigning_kill" = 1 ]; then
    echo "  KILLED BY CODE SIGNING: AMFI still rejects the ad hoc entitlement"
else
    echo "  the probe did not start; output:"
    sed 's/^/    /' "$work/probe.out" | head -20
fi
if command -v log >/dev/null && [ "$launched" = 0 ]; then
    echo "  recent AMFI/taskgated log lines (best effort):"
    log show --last 2m --style compact \
        --predicate 'process == "amfid" OR process == "taskgated" OR sender == "AppleMobileFileIntegrity"' 2>/dev/null \
        | grep -i 'probe' | tail -5 | sed 's/^/    /'
fi

# ---- 5. TSO ---------------------------------------------------------------------------------
hr "Per-thread TSO"
tso_ok=0
tso=$(value tso_set "$work/probe.out")
if [ "$launched" = 0 ]; then
    echo "  not tested: the probe did not launch"
elif [ -z "$tso" ]; then
    echo "  thread_set_x86_64_compat is missing from libsystem_kernel"
else
    echo "  thread_set_x86_64_compat(self, 1) -> $tso$([ "$tso" = 0 ] && echo " (KERN_SUCCESS)")"
    [ "$tso" = 0 ] && tso_ok=1
    off=$(value mp_off.violations "$work/probe.out")
    on=$(value mp_on.violations "$work/probe.out")
    [ -n "$off" ] && echo "  message-passing litmus ($mp_iters iterations): TSO off $off violations, TSO on ${on:-?} violations (thread results $(value mp_on.thread_kr "$work/probe.out"))"
    if [ "$tso_ok" = 1 ] && [ -n "$on" ] && [ "$on" != 0 ]; then
        echo "  WARNING: the call succeeded but the TSO run still saw reordering"
        tso_ok=0
    fi
    [ "$tso_ok" = 1 ] && [ "${off:-0}" = 0 ] && echo "  (0 violations with TSO off too: the litmus didn't catch the M2 reordering this time; the call result is what counts)"
fi

# ---- 6. 4K spawn ----------------------------------------------------------------------------
hr "4K-page spawn"
spawn_ok=0
if [ "$launched" = 0 ]; then
    echo "  not tested: the probe did not launch"
else
    for c in probe probe-x86layout; do
        [ "$c" = probe-x86layout ] && [ "$layout" = 0 ] && { echo "  $c: not built"; continue; }
        attr=$(value "spawn4k.$c.attr" "$work/probe.out")
        src_rc=$(value "spawn4k.$c.rc" "$work/probe.out")
        page=$(value "spawn4k.$c.child_page_size" "$work/probe.out")
        sig=$(value "spawn4k.$c.child_signal" "$work/probe.out")
        line="  $c: set_4k_page_size_np -> ${attr:-?}, posix_spawn -> ${src_rc:-?}"
        [ "$src_rc" = 88 ] && line="$line (EBADMACHO)"
        [ -n "$sig" ] && line="$line, child killed by signal $sig"
        [ -n "$page" ] && line="$line, child page size $page"
        if [ "$src_rc" = 0 ] && [ "$page" = 4096 ]; then
            line="$line  -> OK"
            spawn_ok=1
        fi
        echo "$line"
    done
fi

# ---- verdict --------------------------------------------------------------------------------
hr "Summary"
echo "  SIP disabled: $([ "$sip_off" = 1 ] && echo yes || echo no); amfi_get_out_of_my_way=1: $([ "$amfi_arg" = 1 ] && echo yes || echo no)"
if [ "$launched" = 1 ]; then launch_txt=ok
elif [ "$codesigning_kill" = 1 ]; then launch_txt="killed (CODESIGNING)"
else launch_txt="failed (exit $launch_rc)"
fi
echo "  entitled launch: $launch_txt"
echo "  TSO: $([ "$tso_ok" = 1 ] && echo ok || echo no)   4K spawn: $([ "$spawn_ok" = 1 ] && echo ok || echo no)"
if [ "$launched" = 1 ] && [ "$tso_ok" = 1 ] && [ "$spawn_ok" = 1 ]; then
    echo "VERDICT: PASS - ad hoc signing works with the cross-architecture entitlement; TSO and 4K spawn succeed. Next: LOADER_ADHOC=1 bundle-wine.sh, then tools/run-matrix.sh."
    exit 0
elif [ "$launched" = 1 ]; then
    echo "VERDICT: PARTIAL - the entitled probe launches, but TSO: $([ "$tso_ok" = 1 ] && echo ok || echo FAILED), 4K spawn: $([ "$spawn_ok" = 1 ] && echo ok || echo FAILED). See above."
    exit 1
elif [ "$codesigning_kill" = 1 ]; then
    echo "VERDICT: FAIL - still killed by code signing (CODESIGNING) at launch$([ "$amfi_arg" = 0 ] && echo "; amfi_get_out_of_my_way=1 is not in boot-args"). Ad hoc signing is not enough; wait for the paid team's profile."
    exit 2
else
    echo "VERDICT: FAIL - the entitled probe did not launch (exit $launch_rc, no CODESIGNING report found). See above."
    exit 2
fi
