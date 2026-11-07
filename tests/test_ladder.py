"""Orchestration tests for tests/ladder/run.sh and summarize.py, on Linux, without Wine.

The bundle is a stub: bin/wine prints canned LADDER lines (and compatdb blocks) for each
program, bin/wineserver does nothing, and lib/wine/dxgi|d3d9 hold empty backend trees. What the
stub does per run is set by MOCK_PLAN lines "<glob> <action>", matched against
"<prog>-<arch>-<compatdb value or none>-tso<FEX_HARDWARE_TSO>". Actions: ok, fail, crash, hang,
mismatch (compatdb reports another tree), known (fails seh.stack_overflow).
"""
import csv
import os
import stat
import subprocess
import textwrap

from conftest import ROOT

RUN = os.path.join(ROOT, "tests", "ladder", "run.sh")
SUMMARIZE = os.path.join(ROOT, "tests", "ladder", "summarize.py")

STUB_WINE = r"""#!/bin/bash
case ${1:-} in
    --version) echo "wine-11.20 (stub)"; exit 0 ;;
    wineboot) mkdir -p "$WINEPREFIX/drive_c/windows/system32"; echo stub >"$WINEPREFIX/system.reg"; exit 0 ;;
    reg) exit 0 ;;
esac
exe=$1; shift
prog=${exe%-*}; arch=${exe##*-}; arch=${arch%.exe}
if [ "$prog" = sentinel ]; then
    printf 'LADDER\tINFO\tsentinel.start\tstub\n'
    while [ ! -e "$WINEPREFIX/drive_c/ladder-stop" ]; do sleep 0.1; done
    printf 'LADDER\tPASS\tsentinel.stop\tstub\nLADDER\tDONE\tsentinel\tpass=1 fail=0 skip=0 info=1\n'
    exit 0
fi
key=""; val=none
for kv in $(echo "${WINE_COMPATDB:-}" | tr ';' ' '); do
    case $kv in dxgi=* | d3d9=* | opengl=*) key=${kv%%=*}; val=${kv#*=} ;; esac
done
echo "$prog-$arch-$val-tso${FEX_HARDWARE_TSO:-} $*" >>"$MOCK_CALLS"
action=ok
while read -r pat act; do
    [ -n "$pat" ] || continue
    # shellcheck disable=SC2254
    case "$prog-$arch-$val-tso${FEX_HARDWARE_TSO:-}" in $pat) action=$act; break ;; esac
done <"${MOCK_PLAN:-/dev/null}"
got=$val; [ "$action" = mismatch ] && got=wined3d
echo "compatdb: services.exe [x86_64 (arm64ec)]" >&2
echo "compatdb:   dxgi = dxmt (from /b/lib/wine/dxgi/dxmt), built-in" >&2
echo "compatdb: $exe [$arch]" >&2
[ -n "$key" ] && echo "compatdb:   $key = $got (from /b/lib/wine/$key/$got), WINE_COMPATDB line 1" >&2
printf 'LADDER\tINFO\t%s.env\tbuild=%s\n' "$prog" "$arch"
printf 'LADDER\tPASS\t%s.one\tfine\n' "$prog"
case $action in
    crash) echo "wine: Unhandled page fault" >&2; exit 5 ;;
    hang) sleep 30; exit 0 ;;
    fail) printf 'LADDER\tFAIL\t%s.two\tbroken\nLADDER\tDONE\t%s\tpass=1 fail=1 skip=0 info=1\n' "$prog" "$prog"; exit 1 ;;
    known) printf 'LADDER\tFAIL\tseh.stack_overflow\tstub\nLADDER\tDONE\t%s\tpass=1 fail=1 skip=0 info=1\n' "$prog"; exit 1 ;;
esac
printf 'LADDER\tDONE\t%s\tpass=1 fail=0 skip=0 info=1\n' "$prog"
"""

PROGS = ["threads", "seh", "callbacks", "memory", "ordering", "d3d9", "d3d11", "d3d12", "opengl", "vulkan", "sentinel"]


def make_exec(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def setup(tmp_path, archs, plan="", known=None):
    bundle = tmp_path / "bundle"
    make_exec(str(bundle / "bin" / "wine"), STUB_WINE)
    make_exec(str(bundle / "bin" / "wineserver"), "#!/bin/sh\nexit 0\n")
    for d in ("dxgi/wined3d", "dxgi/dxmt", "d3d9/mtld3d", "d3d9/wined3d"):
        os.makedirs(bundle / "lib" / "wine" / d)
    binp = tmp_path / "out"
    binp.mkdir()
    for a in archs:
        for p in PROGS:
            (binp / ("%s-%s.exe" % (p, a))).write_text("stub")
    (tmp_path / "plan").write_text(textwrap.dedent(plan))
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("LADDER_"):
            del env[k]
    env.update(
        LADDER_BIN=str(binp), LADDER_OUT=str(tmp_path / "results"), LADDER_PREFIX=str(tmp_path / "prefix"),
        LADDER_ARCHS=" ".join(archs), LADDER_HOST_ARM="1", LADDER_TIMEOUT="3", LADDER_DATE="2026-01-01",
        MOCK_PLAN=str(tmp_path / "plan"), MOCK_CALLS=str(tmp_path / "calls"), DISPLAY=":0",
    )
    return bundle, env


def checks(tmp_path):
    with open(tmp_path / "results" / "checks.tsv") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def find(rows, **kw):
    return [r for r in rows if all(r[k] == v for k, v in kw.items())]


def test_bundle_matrix_backends_tso_and_failures(tmp_path):
    bundle, env = setup(tmp_path, ["arm64ec", "x86_64"], plan="""
        d3d11-x86_64-dxmt-tso mismatch
        memory-arm64ec-none-tso crash
        ordering-x86_64-none-tso0 fail
    """)
    p = subprocess.run([RUN, str(bundle)], env=env, capture_output=True, text=True, timeout=300)
    assert p.returncode == 1, p.stdout + p.stderr
    rows = checks(tmp_path)
    calls = (tmp_path / "calls").read_text().splitlines()

    # every graphics backend that exists, and only those
    for prog, vals in (("d3d11", ["wined3d", "dxmt"]), ("d3d12", ["wined3d", "dxmt"]), ("d3d9", ["mtld3d", "wined3d"]),
                       ("opengl", ["egl", "cgl"])):
        for v in vals:
            assert any(c.startswith("%s-x86_64-%s-tso " % (prog, v)) for c in calls), (prog, v)
    assert not any("-dxvk-" in c for c in calls)
    summary = (tmp_path / "results" / "summary.md").read_text()
    assert "dxgi tree dxvk: not in" in summary

    # compatdb verification
    bad = find(rows, phase="serial", arch="x86_64", prog="d3d11", backend="dxmt", check="d3d11.backend")
    assert bad and bad[0]["status"] == "FAIL" and "dxgi=dxmt (got wined3d)" in bad[0]["detail"]
    good = find(rows, phase="serial", arch="x86_64", prog="d3d11", backend="wined3d", check="d3d11.backend")
    assert good and good[0]["status"] == "PASS"

    # a crash is a failure of the whole program
    crash = find(rows, phase="serial", arch="arm64ec", prog="memory", check="memory.run")
    assert crash and crash[0]["status"] == "FAIL" and crash[0]["detail"].startswith("crash")

    # TSO phase: ordering for x86 builds only, with 1 and 0
    tso = [c for c in calls if c.startswith("ordering-") and ("-tso1" in c or "-tso0" in c)]
    assert sorted({c.split()[0] for c in tso}) == ["ordering-x86_64-none-tso0", "ordering-x86_64-none-tso1"]
    f = find(rows, phase="tso", arch="x86_64", tso="0", check="ordering.two")
    assert f and f[0]["status"] == "FAIL"
    assert "## Ordering under FEX_HARDWARE_TSO=1 and =0" in summary

    # concurrent phase in one server lifetime, then a clean shutdown
    assert find(rows, check="server.one_lifetime")[0]["status"] == "PASS"
    assert find(rows, check="shutdown.kill")[0]["status"] == "PASS"
    assert find(rows, check="prefix.wineboot")[0]["status"] == "PASS"
    assert len({c["arch"] for c in find(rows, phase="concurrent") if c["arch"] != "-"}) == 2
    assert "## Verdict: FAIL" in summary


def test_known_failure_passes_and_hang_times_out(tmp_path):
    bundle, env = setup(tmp_path, ["x86_64"], plan="""
        seh-x86_64-none-tso known
    """)
    env.update(LADDER_PHASES="setup serial shutdown", LADDER_GFX="")
    known = tmp_path / "known.txt"
    p = subprocess.run([RUN, str(bundle)], env=env, capture_output=True, text=True, timeout=300)
    assert p.returncode == 1   # the repo's known-failures.txt only covers wine-9.0
    known.write_text("# comment\nwine-11.20*\tx86_64\tseh.stack_overflow\tstub reason\n")
    q = subprocess.run(["python3", SUMMARIZE, str(tmp_path / "results"), "--known", str(known)], capture_output=True, text=True)
    assert q.returncode == 0, q.stdout
    x = find(checks(tmp_path), check="seh.stack_overflow")[0]
    assert x["status"] == "XFAIL" and "stub reason" in x["detail"]

    (tmp_path / "plan").write_text("threads-x86_64-none-tso hang\n")
    env.update(LADDER_PROGS="threads")
    p = subprocess.run([RUN, str(bundle)], env=env, capture_output=True, text=True, timeout=300)
    assert p.returncode == 1
    t = find(checks(tmp_path), check="threads.run")[0]
    assert t["status"] == "FAIL" and t["detail"].startswith("timeout")
