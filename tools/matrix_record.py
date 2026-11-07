#!/usr/bin/env python3
"""Record one tools/run-matrix.sh run in its mpbench JSON and print its matrix.tsv line.

    matrix_record.py --json OUT.json --status ok|crash|timeout --exit N --seconds S --log LOG
                     --runtime R --arch A --part P [--want-dxgi IMPL]

If mpbench wrote no JSON (crash, timeout), a stub results file with no results is written so
bench/compare.py still gets a column for the run. A JSON that exists is kept and gains a "run"
object; ok with no JSON becomes "no-json", and a crash with JSON keeps its results.

--want-dxgi checks the compatdb.so block that mpbench's own process wrote to the log
("compatdb: mpbench-x86_64.exe [x86_64 (arm64ec)]" then "compatdb: dxgi = dxmt (from ...)"):
the run is marked "confirmed", or "mismatch" with the tree it actually got, or "unchecked"
when the log has no compatdb block for mpbench.

Scenes mpbench reported as skipped ("unit": "skipped", e.g. "d3d12.dll not available" on a
Direct3D tree without d3d12.dll) are not failures: they are listed in run["not_available"]
and in the last matrix.tsv column ("gpu12: d3d12.dll not available"), which run-matrix.sh
puts under "Not available on this backend" in summary.md. Standard library only.
"""
import argparse
import json
import os
import re

HEADER = re.compile(r"compatdb:\s+([^=\[]+?)\s+\[([^\]]*)\]")
DXGI = re.compile(r"compatdb:\s+dxgi = (\S+)")


def compatdb_dxgi(log):
    """(dxgi tree, compatdb arch) from the block mpbench's process logged, or (None, None)."""
    current, got, arch = None, None, None
    try:
        with open(log, errors="replace") as f:
            for line in f:
                m = HEADER.search(line)
                if m:
                    current = m.group(1)
                    if "mpbench" in current.lower():
                        arch = m.group(2)
                    continue
                m = DXGI.search(line)
                if m and current and "mpbench" in current.lower():
                    got = m.group(1).rstrip(",")
    except OSError:
        pass
    return got, arch


def not_available(results):
    """{api prefix: reason} for skipped scenes, e.g. {"gpu12": "d3d12.dll not available"}."""
    out = {}
    for r in results:
        if r.get("unit") == "skipped":
            api = r.get("name", "?").split(".")[0]
            reason = str(r.get("skipped") or "not available")
            if reason not in out.setdefault(api, []):
                out[api].append(reason)
    return {api: "; ".join(reasons) for api, reasons in out.items()}


def main():
    ap = argparse.ArgumentParser()
    for k in ("json", "status", "log", "runtime", "arch", "part"):
        ap.add_argument("--" + k, required=True)
    ap.add_argument("--exit", type=int, required=True)
    ap.add_argument("--seconds", type=int, required=True)
    ap.add_argument("--want-dxgi")
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    data = None
    if os.path.exists(a.json):
        try:
            with open(a.json) as f:
                data = json.load(f)
            if data.get("bench") != "mpbench":
                data = None
        except (OSError, ValueError):
            data = None
    status = a.status
    if data is None:
        if status == "ok":
            status = "no-json"
        data = {"bench": "mpbench", "version": 2, "pe_arch": a.arch, "note": a.note, "results": []}

    run = {"status": status, "exit_code": a.exit, "seconds": a.seconds, "runtime": a.runtime,
           "part": a.part, "log": os.path.relpath(a.log, os.path.dirname(a.json))}
    if a.want_dxgi:
        got, arch = compatdb_dxgi(a.log)
        check = "unchecked" if got is None else ("confirmed" if got == a.want_dxgi else "mismatch")
        run["backend"] = {"want": a.want_dxgi, "got": got, "check": check, "compatdb_arch": arch}
    na = not_available(data.get("results", []))
    if na:
        run["not_available"] = na
    data["run"] = run
    with open(a.json, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")

    pingpong = next((r for r in data.get("results", []) if r.get("name") == "cpu.pingpong"), None)
    errors = pingpong.get("ordering_errors", "") if pingpong else ""
    backend = run.get("backend", {}).get("check", "")
    na_text = "; ".join(f"{api}: {why}" for api, why in na.items()).replace("\t", " ")
    print("\t".join(str(x) for x in (os.path.basename(a.json), a.runtime, a.arch, a.part, status,
                                      a.exit, a.seconds, len(data.get("results", [])), errors, backend, na_text)))


if __name__ == "__main__":
    main()
