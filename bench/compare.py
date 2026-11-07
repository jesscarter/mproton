#!/usr/bin/env python3
"""Print a markdown table comparing several mpbench results JSON files.

    bench/compare.py results/a.json results/b.json [--labels A,B] [--relative] [--scenes cpu,gpu12]

One column per file, one row per scene. Device rows show the adapter, feature-probe rows show
PASS/FAIL with passes/s, cpu.pingpong shows its ordering-error count. --relative adds each
value's ratio to the first column. Version 1 files (gpu.* scene names) are read as gpu11.*.

--highlight (used by tools/run-matrix.sh) marks problems in bold: ordering_errors > 0, error and
FAIL cells, and scenes that ran in another column but not in this one ("not run"). Files written
by run-matrix carry a "run" object (status crash/timeout/no-json, exit code, compatdb backend
check); with --highlight it becomes a "run" row, and a "Flags" list follows the table.
Scenes mpbench skipped because their API's DLL is missing ("unit": "skipped") show as
"n/a: <reason>": not available on that backend, which is not a problem and is never flagged.
Standard library only.
"""
import argparse
import json
import os
import sys

ORDER = ["cpu.scalar", "cpu.vector", "cpu.memcpy", "cpu.atomic", "cpu.pingpong",
         "gpu11.device", "gpu11.drawcalls", "gpu11.fill",
         "gpu12.device", "gpu12.present", "gpu12.drawcalls", "gpu12.fill",
         "gpu12.features.gs", "gpu12.features.so", "gpu12.features.tess",
         "vk.device", "vk.drawcalls", "vk.fill"]
LEGACY = {"gpu.device": "gpu11.device", "gpu.drawcalls": "gpu11.drawcalls", "gpu.fill": "gpu11.fill"}
UNITS = {"Krt/s": "K round-trips/s", "GFLOP/s(x2)": "GFLOP/s x2"}


def load(path):
    with open(path) as f:
        data = json.load(f)
    if data.get("bench") != "mpbench":
        sys.exit(f"{path}: not an mpbench results file")
    rows = {}
    for r in data.get("results", []):
        rows[LEGACY.get(r["name"], r["name"])] = r
    return data, rows


def num(v):
    if v >= 1000:
        return f"{v:,.0f}"
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def short(s, n=70):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "…"


def feature_level(fl):
    try:
        v = int(fl, 16)
        return f"{v >> 12}_{(v >> 8) & 0xf}"
    except (TypeError, ValueError):
        return str(fl)


def run_status(data):
    """Text for a run-matrix "run" object, and whether it is a problem."""
    run = data.get("run")
    if not run:
        return "ok", False
    st = run.get("status", "?")
    txt = st
    if st == "crash":
        txt = f"crash (exit {run.get('exit_code', '?')})"
    elif st == "timeout":
        txt = f"timeout ({run.get('seconds', '?')} s)"
    bad = st != "ok"
    be = run.get("backend")
    if be and be.get("check") == "mismatch":
        txt += f", backend {be.get('got') or 'unknown'} not {be.get('want')}"
        bad = True
    return txt, bad


def cell(name, r):
    """(text, numeric value or None) for one result row."""
    if r is None:
        return "–", None
    if r.get("unit") == "skipped":
        return "n/a: " + short(r.get("skipped", "not available"), 50), None
    if r.get("unit") == "error" or (r.get("ok") is False and "error" in r and not name.startswith("gpu12.features")):
        return "error: " + short(r.get("error", "?")), None
    if name.endswith(".device"):
        txt = r.get("adapter", "?")
        if "feature_level" in r:
            fl = feature_level(r["feature_level"])
            txt = f"FL {fl} only: {txt}" if r.get("unit") == "fl<11_0" else f"{txt} (FL {fl})"
        if "shader_model" in r:
            txt += f", SM {r['shader_model']}, {r.get('shaders', '?').upper()}"
        if "present_mode" in r:
            txt += f", Vulkan {r.get('api_version', '?')}, {r['present_mode']}"
        return txt, None
    if name.startswith("gpu12.features."):
        if r.get("ok"):
            return f"PASS {num(r['value'])}", r["value"]
        if "error" in r:
            return "FAIL: " + short(r["error"], 60), None
        return "FAIL (" + short(r.get("check", "?")) + ")", None
    txt = num(r["value"])
    if "ordering_errors" in r:
        txt += f" ({r['ordering_errors']} ordering errors)" if r["ordering_errors"] else " (0 errors)"
    if r.get("WRONG_COUNT"):
        txt += " **WRONG COUNT**"
    return txt, r["value"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--labels", help="comma-separated column labels (default: file names)")
    ap.add_argument("--relative", action="store_true", help="show ratio to the first column")
    ap.add_argument("--scenes", help="comma-separated scene name prefixes to include")
    ap.add_argument("--no-sources", action="store_true", help="omit the list of files/notes under the table")
    ap.add_argument("--highlight", action="store_true",
                    help="bold problems (ordering errors, errors, scenes not run) and list them after the table")
    a = ap.parse_args()

    loaded = [load(p) for p in a.files]
    labels = a.labels.split(",") if a.labels else [os.path.splitext(os.path.basename(p))[0] for p in a.files]
    if len(labels) != len(a.files):
        sys.exit("--labels needs one label per file")
    prefixes = a.scenes.split(",") if a.scenes else None

    names = []
    for _, rows in loaded:
        for n in rows:
            if n not in names:
                names.append(n)
    first_seen = {n: i for i, n in enumerate(names)}
    names.sort(key=lambda n: (ORDER.index(n) if n in ORDER else len(ORDER), first_seen[n]))
    if prefixes:
        names = [n for n in names if any(n.startswith(p) for p in prefixes)]

    def esc(s):
        return s.replace("|", "\\|")

    out = ["| scene | " + " | ".join(esc(l) for l in labels) + " |", "|---" * (len(labels) + 1) + "|"]
    flags = []
    if a.highlight:
        texts = []
        for label, (data, _) in zip(labels, loaded):
            txt, bad = run_status(data)
            texts.append(esc(f"**{txt}**" if bad else txt))
            if bad:
                flags.append(f"{label}: {txt}")
        out.append("| run | " + " | ".join(texts) + " |")
    for n in names:
        unit = ""
        for _, rows in loaded:
            r = rows.get(n)
            if r and r.get("unit") not in (None, "", "error", "ok", "fl<11_0", "skipped"):
                unit = UNITS.get(r["unit"], r["unit"])
                break
        cells = [cell(n, rows.get(n)) for _, rows in loaded]
        base = cells[0][1]
        texts = []
        for i, (txt, v) in enumerate(cells):
            if a.relative and i > 0 and v is not None and base:
                txt += f" (x{v / base:.2f})"
            if a.highlight:
                r = loaded[i][1].get(n)
                problem = None
                if r is None:
                    txt, problem = "not run", "did not run"
                elif r.get("ordering_errors"):
                    txt, problem = f"{txt} ⚠", f"{r['ordering_errors']} ordering errors"
                elif txt.startswith(("error", "FAIL")) or r.get("WRONG_COUNT"):
                    problem = short(txt, 80)
                if problem:
                    txt = f"**{txt}**"
                    flags.append(f"{labels[i]}: {n} {problem}")
            texts.append(esc(txt))
        out.append(f"| {n}" + (f" ({unit})" if unit else "") + " | " + " | ".join(texts) + " |")
    print("\n".join(out))
    if a.highlight:
        print()
        if flags:
            print("Flags:")
            print("\n".join(f"- {esc(f)}" for f in flags))
        else:
            print("Flags: none.")

    if not a.no_sources:
        print()
        for label, path, (data, _) in zip(labels, a.files, loaded):
            bits = [f"pe_arch {data.get('pe_arch', '?')}"]
            for k in ("wine", "host"):
                if data.get(k):
                    bits.append(f"{k} {data[k]}")
            if "frames" in data:
                bits.append(f"{data['frames']} frames at {data.get('width')}x{data.get('height')}")
            if data.get("present_mode", "default") != "default":
                bits.append(f"--present-mode {data['present_mode']}")
            line = f"- **{label}**: `{path}` ({', '.join(bits)})"
            if data.get("note"):
                line += f". {data['note']}"
            print(line)


if __name__ == "__main__":
    main()
