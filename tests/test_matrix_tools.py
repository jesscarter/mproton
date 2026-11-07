"""Orchestration tests for tools/run-matrix.sh and tools/amfi-check.sh, on Linux.

Nothing here runs Wine or touches macOS: the runtimes are stub `wine` scripts that emit canned
mpbench JSON and compatdb log lines, and the macOS commands (csrutil, nvram, clang, codesign,
...) are stubs on PATH. What the stubs do per run is set by MOCK_PLAN lines "<glob> <action>",
matched against "<kind>-tso<FEX_HARDWARE_TSO>-<arch>-<cpu|gpu>-<dxgi>".
"""
import json
import os
import stat
import subprocess
import textwrap

from conftest import ROOT

MATRIX = os.path.join(ROOT, "tools", "run-matrix.sh")
AMFI = os.path.join(ROOT, "tools", "amfi-check.sh")

STUB_WINE = r"""#!/bin/bash
# Stub wine for tests: KIND is rosetta or arm64.
KIND=@KIND@
case ${1:-} in
    --version) echo "wine-@VER@ (stub)"; exit 0 ;;
    wineboot|reg) [ -n "${WINEPREFIX:-}" ] && mkdir -p "$WINEPREFIX/drive_c"; exit 0 ;;
esac
exe=$1; shift
arch=${exe#mpbench-}; arch=${arch%.exe}
part=all out="" ; want=""
while [ $# -gt 0 ]; do
    case $1 in
        --no-gpu) part=cpu ;;
        --no-cpu) part=gpu ;;
        --out) out=$2; shift ;;
    esac
    shift
done
case ${WINE_COMPATDB:-} in *dxgi=*) want=${WINE_COMPATDB##*dxgi=}; want=${want%%;*} ;; esac
got=${want:-dxmt}
if [ "$got" = dxvk ] && [ -z "${MOCK_DXVK_KNOWN:-}" ]; then
    echo "compatdb: warning: WINE_COMPATDB line 1: dxgi: unknown value dxvk" >&2
    got=dxmt
fi
key="$KIND-tso${FEX_HARDWARE_TSO:-}-$arch-$part-${want:-none}"
echo "$key" >> "$MOCK_CALLS"
action=ok
while read -r pat act; do
    [ -n "$pat" ] || continue
    # shellcheck disable=SC2254
    case $key in $pat) action=$act; break ;; esac
done < "${MOCK_PLAN:-/dev/null}"
if [ "$KIND" = arm64 ]; then
    echo "compatdb: services.exe [x86_64 (arm64ec)]" >&2
    echo "compatdb: dxgi = dxmt (from /b/lib/wine/dxgi/dxmt/), built-in" >&2
    echo "compatdb: $exe [$arch]" >&2
    echo "compatdb: dxgi = $got (from /b/lib/wine/dxgi/$got/), WINE_COMPATDB line 1" >&2
fi
case $action in
    crash) echo "wine: Unhandled page fault" >&2; exit 5 ;;
    hang) sleep 60; exit 0 ;;
    nojson) exit 0 ;;
esac
errors=0; [ "$action" = ordering ] && errors=7
if [ "$part" = cpu ]; then
    res='{"name": "cpu.scalar", "value": 150.0, "unit": "Miter/s"},
         {"name": "cpu.pingpong", "value": 9000.0, "unit": "Krt/s", "ordering_errors": '$errors'}'
else
    res='{"name": "gpu11.device", "value": 1, "unit": "ok", "adapter": "Apple M2 ('$got')", "feature_level": "0xb000"},
         {"name": "gpu11.fill", "value": 300.0, "unit": "fps"}'
    if [ "$action" = nod3d12 ]; then   # a Direct3D tree without d3d12.dll: mpbench skips D3D12
        res="$res"',
         {"name": "gpu12.device", "value": 0, "unit": "skipped", "skipped": "d3d12.dll not available"},
         {"name": "gpu12.features.tess", "value": 0, "unit": "skipped", "skipped": "d3d12.dll not available"}'
    elif [ "$action" != notess ]; then
        res="$res"',
         {"name": "gpu12.features.tess", "value": 50.0, "unit": "passes/s", "ok": true}'
    fi
fi
cat > "$out" <<EOF
{"bench": "mpbench", "version": 2, "pe_arch": "$arch", "wine": "@VER@", "results": [$res]}
EOF
"""


def make_exec(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def setup_matrix(tmp_path, plan="", dxvk_tree=False):
    bundle = tmp_path / "bundle"
    make_exec(str(bundle / "bin" / "wine"), STUB_WINE.replace("@KIND@", "arm64").replace("@VER@", "11.20"))
    make_exec(str(bundle / "bin" / "wineserver"), "#!/bin/sh\nexit 0\n")
    for impl in ["wined3d", "dxmt"] + (["dxvk"] if dxvk_tree else []):
        (bundle / "lib" / "wine" / "dxgi" / impl).mkdir(parents=True)
    app = tmp_path / "Wine Stable.app"
    make_exec(str(app / "Contents" / "Resources" / "wine" / "bin" / "wine"),
              STUB_WINE.replace("@KIND@", "rosetta").replace("@VER@", "10.0"))
    make_exec(str(app / "Contents" / "Resources" / "wine" / "bin" / "wineserver"), "#!/bin/sh\nexit 0\n")
    mp = tmp_path / "out"
    mp.mkdir()
    for arch in ("x86_64", "i686", "aarch64", "arm64ec"):
        (mp / f"mpbench-{arch}.exe").write_bytes(b"MZ")
    (tmp_path / "plan").write_text(textwrap.dedent(plan))
    return bundle, app, mp


def run_matrix(tmp_path, bundle, app, mp, **env):
    res = tmp_path / "results"
    e = dict(os.environ, MPBENCH_DIR=str(mp), MATRIX_OUT=str(res), MATRIX_PREFIXES=str(tmp_path / "prefixes"),
             MATRIX_DATE="2026-11-08", MOCK_PLAN=str(tmp_path / "plan"), MOCK_CALLS=str(tmp_path / "calls"),
             TMPDIR=str(tmp_path))
    e.update(env)
    p = subprocess.run(["bash", MATRIX, str(bundle), str(app)], env=e, capture_output=True, text=True, timeout=120)
    assert p.returncode == 0, p.stdout + p.stderr
    return type(res)(e["MATRIX_OUT"]), p.stdout


def tsv_rows(res):
    lines = (res / "matrix.tsv").read_text().splitlines()
    head = lines[0].split("\t")
    return [dict(zip(head, l.split("\t"))) for l in lines[1:]]


def test_matrix_runs_every_combination(tmp_path):
    bundle, app, mp = setup_matrix(tmp_path)
    res, _ = run_matrix(tmp_path, bundle, app, mp)
    calls = (tmp_path / "calls").read_text().split()
    # rosetta: 2 archs x (cpu + wined3d); tso1: 4 archs x (cpu, wined3d, dxmt); tso0: 2 archs x 3
    assert len(calls) == 4 + 12 + 6
    assert "rosetta-tso-x86_64-gpu-none" in calls          # no compatdb rule on Rosetta
    assert "arm64-tso1-arm64ec-gpu-dxmt" in calls
    assert "arm64-tso0-i686-cpu-none" in calls
    assert not any(c.startswith("arm64-tso0-aarch64") for c in calls)
    assert not any(c.startswith("rosetta-tso-arm64ec") for c in calls)
    rows = tsv_rows(res)
    ran = [r for r in rows if not r["status"].startswith("skipped")]
    assert len(ran) == 22 and all(r["status"] == "ok" for r in ran)
    assert all(r["backend"] == "confirmed" for r in ran if r["runtime"] != "rosetta" and r["part"] != "cpu")
    skipped = [r for r in rows if r["status"].startswith("skipped")]
    assert any("dxvk" in r["status"] for r in skipped)    # no dxvk tree in this bundle
    # fresh prefix per runtime
    assert sorted(os.listdir(tmp_path / "prefixes")) == ["arm64-tso0", "arm64-tso1", "rosetta"]
    data = json.loads((res / "arm64-tso1-x86_64-dxmt.json").read_text())
    assert data["run"]["status"] == "ok"
    assert data["run"]["backend"] == {"want": "dxmt", "got": "dxmt", "check": "confirmed", "compatdb_arch": "x86_64"}
    summary = (res / "summary.md").read_text()
    assert "# M1 matrix, 2026-11-08" in summary
    assert "| scene | rosetta | TSO=1 | TSO=0 |" in summary
    assert "| scene | rosetta wined3d | TSO=1 wined3d | TSO=1 dxmt + vkd3d-proton | TSO=0 wined3d | TSO=0 dxmt + vkd3d-proton |" in summary
    assert "wine-10.0 (stub)" in summary and "wine-11.20 (stub)" in summary


def test_matrix_records_failures_and_carries_on(tmp_path):
    bundle, app, mp = setup_matrix(tmp_path, plan="""
        arm64-tso1-x86_64-gpu-dxmt crash
        arm64-tso1-i686-gpu-wined3d hang
        arm64-tso0-x86_64-cpu-none ordering
        arm64-tso1-aarch64-gpu-dxmt notess
        rosetta-tso-i686-cpu-none nojson
    """)
    res, out = run_matrix(tmp_path, bundle, app, mp, MATRIX_TIMEOUT="2")
    rows = {r["file"]: r for r in tsv_rows(res)}
    assert rows["arm64-tso1-x86_64-dxmt.json"]["status"] == "crash"
    assert rows["arm64-tso1-x86_64-dxmt.json"]["exit"] == "5"
    assert rows["arm64-tso1-i686-wined3d.json"]["status"] == "timeout"
    assert rows["arm64-tso1-i686-wined3d.json"]["exit"] == "124"
    assert rows["rosetta-i686-cpu.json"]["status"] == "no-json"
    assert rows["arm64-tso0-x86_64-cpu.json"]["ordering_errors"] == "7"
    # runs after the failures still happened
    assert rows["arm64-tso1-i686-dxmt.json"]["status"] == "ok"
    assert rows["arm64-tso0-i686-dxmt.json"]["status"] == "ok"
    stub = json.loads((res / "arm64-tso1-x86_64-dxmt.json").read_text())
    assert stub["results"] == [] and stub["run"]["status"] == "crash"
    assert (res / "logs" / "arm64-tso1-x86_64-dxmt.log").read_text().count("Unhandled page fault") == 1
    summary = (res / "summary.md").read_text()
    problems = summary.split("## Problems")[1].split("## x86_64")[0]
    assert "**arm64-tso1-x86_64-dxmt.json**: crash (exit 5" in problems
    assert "**arm64-tso1-i686-wined3d.json**: timeout" in problems
    assert "**arm64-tso0-x86_64-cpu.json**: cpu.pingpong 7 ordering errors" in problems
    assert "**rosetta-i686-cpu.json**: no-json" in problems
    assert "9,000 (7 ordering errors) ⚠**" in summary
    assert "- TSO=0: cpu.pingpong 7 ordering errors" in summary
    assert "**crash (exit 5)**" in summary and "**not run**" in summary
    assert "- TSO=1 dxmt + vkd3d-proton: gpu12.features.tess did not run" in summary   # aarch64 notess
    assert "- TSO=1 dxmt + vkd3d-proton: gpu11.fill did not run" in summary            # x86_64 crash


def test_matrix_flags_backend_mismatch(tmp_path):
    bundle, app, mp = setup_matrix(tmp_path, dxvk_tree=True)
    res, _ = run_matrix(tmp_path, bundle, app, mp, MATRIX_RUNTIMES="arm64-tso1", MATRIX_ARCHS="x86_64")
    rows = {r["file"]: r for r in tsv_rows(res)}
    assert rows["arm64-tso1-x86_64-dxvk.json"]["backend"] == "mismatch"
    assert rows["arm64-tso1-x86_64-dxmt.json"]["backend"] == "confirmed"
    data = json.loads((res / "arm64-tso1-x86_64-dxvk.json").read_text())
    assert data["run"]["backend"]["got"] == "dxmt"
    summary = (res / "summary.md").read_text()
    assert "backend dxmt not dxvk" in summary
    res2, _ = run_matrix(tmp_path, bundle, app, mp, MATRIX_RUNTIMES="arm64-tso1", MATRIX_ARCHS="x86_64",
                         MOCK_DXVK_KNOWN="1", MATRIX_OUT=str(tmp_path / "results2"))
    rows = {r["file"]: r for r in tsv_rows(res2)}
    assert rows["arm64-tso1-x86_64-dxvk.json"]["backend"] == "confirmed"


def test_matrix_skipped_scenes_are_not_failures(tmp_path):
    bundle, app, mp = setup_matrix(tmp_path, plan="""
        arm64-tso1-x86_64-gpu-wined3d nod3d12
    """)
    res, out = run_matrix(tmp_path, bundle, app, mp, MATRIX_RUNTIMES="arm64-tso1", MATRIX_ARCHS="x86_64")
    rows = {r["file"]: r for r in tsv_rows(res)}
    row = rows["arm64-tso1-x86_64-wined3d.json"]
    assert row["status"] == "ok" and row["not_available"] == "gpu12: d3d12.dll not available"
    assert rows["arm64-tso1-x86_64-dxmt.json"]["not_available"] == ""
    assert "not available: gpu12: d3d12.dll not available" in out
    data = json.loads((res / "arm64-tso1-x86_64-wined3d.json").read_text())
    assert data["run"]["not_available"] == {"gpu12": "d3d12.dll not available"}
    summary = (res / "summary.md").read_text()
    problems = summary.split("## Problems")[1].split("## Not available on this backend")[0]
    assert "None at the run level" in problems
    na = summary.split("## Not available on this backend")[1].split("## x86_64")[0]
    assert "- arm64-tso1-x86_64-wined3d.json: gpu12: d3d12.dll not available" in na
    table = summary.split("### x86_64 GPU")[1]
    assert "| gpu12.features.tess (passes/s) | n/a: d3d12.dll not available | PASS 50.0 |" in table
    assert "| gpu12.device | n/a: d3d12.dll not available | " in table
    flags = table.split("Flags:")[1].split("\n\n")[0]
    assert "TSO=1 wined3d" not in flags    # n/a is not a problem


def test_matrix_skips_missing_runtime(tmp_path):
    bundle, app, mp = setup_matrix(tmp_path)
    e = dict(os.environ, MPBENCH_DIR=str(mp), MATRIX_OUT=str(tmp_path / "r"), MATRIX_PREFIXES=str(tmp_path / "p"),
             MOCK_CALLS=str(tmp_path / "calls"), TMPDIR=str(tmp_path))
    p = subprocess.run(["bash", MATRIX, "-", str(app / "Contents" / "Resources" / "wine" / "bin")],
                       env=e, capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    rows = tsv_rows(tmp_path / "r")
    assert [r["runtime"] for r in rows if r["status"] == "skipped: runtime not given"] == ["arm64-tso1", "arm64-tso0"]
    assert len([r for r in rows if r["status"] == "ok"]) == 4


# ---- amfi-check -------------------------------------------------------------------------------

STUB_PROBE = r"""#!/bin/bash
# Stub probe; MOCK_PROBE picks the outcome.
case ${MOCK_PROBE:-pass} in
    killed)
        case $0 in *noent) ;; *)
            mkdir -p "$AMFI_DIAG_DIR"
            echo '{"exception":{"signal":"SIGKILL (Code Signature Invalid)"},"termination":{"namespace":"CODESIGNING","indicator":"Taskgated Invalid Signature"}}' \
                > "$AMFI_DIAG_DIR/probe-2026-11-08-101010.ips"
            kill -9 $$ ;;
        esac ;;
esac
echo "RESULT launched=1"
case $0 in
    *noent) echo "RESULT tso_set=5"; echo "RESULT spawn4k.probe-noent.attr=0"; echo "RESULT spawn4k.probe-noent.rc=88"; exit 0 ;;
esac
if [ "${MOCK_PROBE:-pass}" = notso ]; then echo "RESULT tso_set=5"; else echo "RESULT tso_set=0"; fi
echo "RESULT mp_off.violations=12"
echo "RESULT mp_on.violations=0"
echo "RESULT mp_on.thread_kr=0,0"
for c in probe probe-x86layout; do
    echo "RESULT spawn4k.$c.attr=0"
    if [ "$c" = probe ] || [ "${MOCK_LAYOUT_SPAWN:-ok}" != ok ]; then
        echo "RESULT spawn4k.$c.rc=88"
    else
        echo "RESULT spawn4k.$c.rc=0"; echo "RESULT spawn4k.$c.child_exit=0"; echo "RESULT spawn4k.$c.child_page_size=4096"
    fi
done
echo "RESULT done=1"
"""


def amfi_env(tmp_path, **extra):
    bin_ = tmp_path / "bin"
    make_exec(str(bin_ / "csrutil"), "#!/bin/sh\necho 'System Integrity Protection status: disabled.'\n")
    make_exec(str(bin_ / "nvram"), "#!/bin/sh\nprintf 'boot-args\\t%s\\n' \"$MOCK_BOOTARGS\"\n")
    make_exec(str(bin_ / "sw_vers"), "#!/bin/sh\necho 'ProductVersion:\t27.2'\n")
    (tmp_path / "probe-stub").write_text(STUB_PROBE)
    make_exec(str(bin_ / "clang"), f"""#!/bin/bash
while [ $# -gt 0 ]; do [ "$1" = -o ] && out=$2; shift; done
cp {tmp_path}/probe-stub "$out"; chmod +x "$out"
""")
    make_exec(str(bin_ / "codesign"), """#!/bin/bash
case " $* " in *" -d "*) echo '<plist><dict><key>com.apple.developer.cross-architecture-support</key><true/></dict></plist>' ;; esac
exit 0
""")
    e = dict(os.environ, PATH=f"{bin_}:{os.environ['PATH']}", AMFI_CHECK_WORK=str(tmp_path / "work"),
             AMFI_DIAG_DIR=str(tmp_path / "diag"), AMFI_TIMEOUT="10", MOCK_BOOTARGS="amfi_get_out_of_my_way=1")
    e.update(extra)
    return e


def run_amfi(tmp_path, **extra):
    p = subprocess.run(["bash", AMFI], env=amfi_env(tmp_path, **extra), capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout


def test_amfi_pass(tmp_path):
    rc, out = run_amfi(tmp_path)
    assert rc == 0, out
    assert "amfi_get_out_of_my_way=1 is set" in out
    assert "sudo bputil -d" in out
    assert "probe-x86layout: set_4k_page_size_np -> 0, posix_spawn -> 0, child page size 4096  -> OK" in out
    assert "probe: set_4k_page_size_np -> 0, posix_spawn -> 88 (EBADMACHO)" in out
    assert "TSO off 12 violations, TSO on 0 violations" in out
    assert "posix_spawn -> 88 (88 = EBADMACHO, expected" in out     # control run
    assert out.rstrip().splitlines()[-1].startswith("VERDICT: PASS")


def test_amfi_codesigning_kill(tmp_path):
    rc, out = run_amfi(tmp_path, MOCK_PROBE="killed", MOCK_BOOTARGS="")
    assert rc == 2, out
    assert "exit 137 (signal 9)" in out
    assert '"namespace":"CODESIGNING"' in out.replace(" ", "")
    assert "amfi_get_out_of_my_way=1 is NOT set" in out
    last = out.rstrip().splitlines()[-1]
    assert last.startswith("VERDICT: FAIL - still killed by code signing") and "not in boot-args" in last


def test_amfi_partial(tmp_path):
    rc, out = run_amfi(tmp_path, MOCK_PROBE="notso", MOCK_LAYOUT_SPAWN="fail")
    assert rc == 1, out
    assert out.rstrip().splitlines()[-1] == \
        "VERDICT: PARTIAL - the entitled probe launches, but TSO: FAILED, 4K spawn: FAILED. See above."
