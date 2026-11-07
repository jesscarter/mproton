#!/usr/bin/env python3
"""Turn a tests/ladder/run.sh output directory into checks.tsv and summary.md.

Usage: summarize.py OUT_DIR [--known tests/ladder/known-failures.txt]

Reads OUT_DIR/runs.tsv (one row per program run, written by run.sh), OUT_DIR/env.tsv and the
logs the rows point at. Every log line of the form

    LADDER<TAB>STATUS<TAB>program.check<TAB>detail

is one check. On top of those, each run adds:
  - <prog>.run FAIL when the program crashed, hung or never printed its DONE line
  - <prog>.backend FAIL when the run had a compatdb rule (dxgi=, d3d9=, opengl=) and the
    "compatdb:" block the program's own process logged names a different value
A FAIL that matches a line of the known-failures file becomes XFAIL.

Exit status 1 if any FAIL is left (an unexpected failure), else 0. Standard library only.
"""
import argparse
import fnmatch
import os
import re
import sys
from collections import OrderedDict

STATUSES = ("PASS", "FAIL", "XFAIL", "SKIP", "INFO")
HEADER = re.compile(r"compatdb:\s+([^=\[]+?)\s+\[([^\]]*)\]")
VALUE = re.compile(r"compatdb:\s+(dxgi|d3d9|opengl) = (\S+)")


def read_tsv(path):
    with open(path, errors="replace") as f:
        lines = [l.rstrip("\n") for l in f if l.strip()]
    head = lines[0].split("\t")
    return [dict(zip(head, (l.split("\t") + [""] * len(head))[: len(head)])) for l in lines[1:]]


def read_env(path):
    env, notes = OrderedDict(), []
    if os.path.exists(path):
        with open(path, errors="replace") as f:
            for line in f:
                k, _, v = line.rstrip("\n").partition("\t")
                if k == "note":
                    notes.append(v)
                elif k:
                    env[k] = v
    return env, notes


def parse_log(path):
    """[(status, check, detail)], done_seen"""
    checks, done = [], False
    try:
        with open(path, errors="replace") as f:
            for line in f:
                if not line.startswith("LADDER\t"):
                    continue
                parts = line.rstrip("\r\n").split("\t")
                if len(parts) < 3:
                    continue
                if parts[1] == "DONE":
                    done = True
                elif parts[1] in STATUSES:
                    checks.append((parts[1], parts[2], parts[3] if len(parts) > 3 else ""))
    except OSError:
        pass
    return checks, done


def compatdb_values(path, exe):
    """{key: value} from the compatdb block logged by the process running `exe`, or None."""
    current, got, seen = None, {}, False
    try:
        with open(path, errors="replace") as f:
            for line in f:
                m = HEADER.search(line)
                if m:
                    current = m.group(1).strip()
                    if current.lower() == exe.lower():
                        seen = True
                    continue
                m = VALUE.search(line)
                if m and current and current.lower() == exe.lower():
                    got[m.group(1)] = m.group(2).rstrip(",")
    except OSError:
        return None
    return got if seen else None


def load_known(path):
    known = []
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 4:
                    known.append(tuple(p.strip() for p in parts[:4]))
    return known


def known_reason(known, wine_version, arch, check):
    for wv, a, c, reason in known:
        if fnmatch.fnmatch(wine_version, wv) and fnmatch.fnmatch(arch, a) and fnmatch.fnmatch(check, c):
            return reason
    return None


def collect(out, known_path=None):
    env, notes = read_env(os.path.join(out, "env.tsv"))
    known = load_known(known_path)
    runs = read_tsv(os.path.join(out, "runs.tsv"))
    wine_version = env.get("wine_version", "")
    checks, skipped_runs = [], []
    for r in runs:
        if r["status"].startswith("skipped"):
            skipped_runs.append(r)
            continue
        base = {k: r[k] for k in ("phase", "arch", "prog", "backend", "tso")}
        log = os.path.join(out, r["log"])
        found, done = parse_log(log)
        for status, check, detail in found:
            checks.append(dict(base, status=status, check=check, detail=detail))
        if r["status"] in ("crash", "timeout") or not done:
            tail = ""
            try:
                with open(log, errors="replace") as f:
                    lines = [l.strip() for l in f if l.strip() and not l.startswith("LADDER")]
                tail = " | ".join(lines[-3:])[:300]
            except OSError:
                pass
            checks.append(dict(base, status="FAIL", check=r["prog"] + ".run",
                               detail="%s (exit %s after %ss)%s" % (r["status"], r["exit"], r["seconds"],
                                                                    "; log tail: " + tail if tail else "")))
        rule = r.get("rule", "")
        if rule and env.get("compatdb") == "1":
            exe = "%s-%s.exe" % (r["prog"], r["arch"])
            want = dict(kv.split("=", 1) for kv in rule.split(";") if "=" in kv and not kv.startswith("exe="))
            got = compatdb_values(log, exe)
            if got is None:
                checks.append(dict(base, status="INFO", check=r["prog"] + ".backend",
                                   detail="no compatdb block for %s in the log; backend not verified" % exe))
            else:
                bad = ["%s=%s (got %s)" % (k, v, got.get(k, "nothing")) for k, v in want.items() if got.get(k) != v]
                checks.append(dict(base, status="FAIL" if bad else "PASS", check=r["prog"] + ".backend",
                                   detail=("compatdb gave a different tree: " + ", ".join(bad)) if bad
                                   else "compatdb applied " + rule))
    for c in checks:
        if c["status"] == "FAIL":
            reason = known_reason(known, wine_version, c["arch"], c["check"])
            if reason:
                c["status"], c["detail"] = "XFAIL", "%s [known: %s]" % (c["detail"], reason)
    return env, notes, runs, checks, skipped_runs


def cell(cs):
    if not cs:
        return "-"
    n = {s: sum(1 for c in cs if c["status"] == s) for s in STATUSES}
    run_fail = [c for c in cs if c["check"].endswith(".run") and c["status"] in ("FAIL", "XFAIL")]
    if run_fail:
        word = run_fail[0]["detail"].split(" ")[0]
        return "**%s**" % word.upper()
    total = n["PASS"] + n["FAIL"] + n["XFAIL"]
    if n["FAIL"]:
        return "**FAIL %d/%d**" % (n["FAIL"], total)
    if n["XFAIL"]:
        return "ok %d/%d, %d known" % (n["PASS"], total, n["XFAIL"])
    if total == 0:
        return "skip" if n["SKIP"] else "-"
    return "ok %d%s" % (n["PASS"], " (%d skip)" % n["SKIP"] if n["SKIP"] else "")


def matrix(checks, phase, archs):
    rows = OrderedDict()
    for c in checks:
        if c["phase"] != phase:
            continue
        if c["arch"] == "-":
            continue
        key = c["prog"] + (" / " + c["backend"] if c["backend"] else "")
        rows.setdefault(key, {}).setdefault(c["arch"], []).append(c)
    if not rows:
        return []
    out = ["| program | " + " | ".join(archs) + " |", "|---|" + "---|" * len(archs)]
    for key, by_arch in rows.items():
        out.append("| %s | %s |" % (key, " | ".join(cell(by_arch.get(a, [])) for a in archs)))
    return out


def tso_table(checks, archs):
    cols = [(a, t) for t in ("1", "0") for a in archs if any(
        c["phase"] == "tso" and c["arch"] == a and c["tso"] == t for c in checks)]
    if not cols:
        return []
    names = OrderedDict()
    for c in checks:
        if c["phase"] == "tso" and c["status"] in ("PASS", "FAIL", "XFAIL", "INFO") and not c["check"].endswith(("env", "config")):
            names.setdefault(c["check"], {})[(c["arch"], c["tso"])] = c
    out = ["| check | " + " | ".join("%s TSO=%s" % col for col in cols) + " |", "|---|" + "---|" * len(cols)]
    for name, by in names.items():
        cells = []
        for col in cols:
            c = by.get(col)
            if not c:
                cells.append("-")
                continue
            m = re.search(r"(\d+)/(\d+)", c["detail"])
            frac = m.group(0) if m else ""
            cells.append(("**FAIL** " if c["status"] == "FAIL" else c["status"].lower() + " ") + frac)
        out.append("| %s | %s |" % (name.split(".", 1)[-1], " | ".join(cells)))
    return out


def where(c):
    parts = [c["phase"], c["arch"], c["prog"]]
    if c["backend"]:
        parts.append(c["backend"])
    if c["tso"]:
        parts.append("TSO=" + c["tso"])
    return " ".join(p for p in parts if p and p != "-")


def write(out, env, notes, runs, checks, skipped_runs):
    archs = env.get("archs", "").split() or sorted({c["arch"] for c in checks if c["arch"] != "-"})
    with open(os.path.join(out, "checks.tsv"), "w") as f:
        f.write("phase\tarch\tprog\tbackend\ttso\tstatus\tcheck\tdetail\n")
        for c in checks:
            f.write("\t".join(c[k] for k in ("phase", "arch", "prog", "backend", "tso", "status", "check", "detail")) + "\n")

    n = {s: sum(1 for c in checks if c["status"] == s) for s in STATUSES}
    fails = [c for c in checks if c["status"] == "FAIL"]
    L = ["# Correctness ladder, %s" % env.get("date", "?"), ""]
    L.append("Generated by `tests/ladder/run.sh`. Every check is in `checks.tsv`, every run in `runs.tsv`, logs in `logs/`.")
    L.append("")
    L.append("- **Wine:** `%s` (%s)" % (env.get("wine_version", "?"), env.get("wine", "?")))
    L.append("- **Bundle:** %s; compatdb backend selection: %s" % (env.get("bundle", "?"), "yes" if env.get("compatdb") == "1" else "no (built-in graphics stack)"))
    L.append("- **Host:** %s" % env.get("host", "?"))
    L.append("- **Archs:** %s" % " ".join(archs))
    for note in notes:
        L.append("- %s" % note)
    L.append("")
    verdict = "PASS" if not fails else "FAIL"
    L.append("## Verdict: %s" % verdict)
    L.append("")
    L.append("%d checks: %d pass, %d fail, %d known failures, %d skipped, %d info." % (
        len(checks), n["PASS"], n["FAIL"], n["XFAIL"], n["SKIP"], n["INFO"]))
    L.append("")
    if fails:
        L.append("### Failures")
        L.append("")
        for c in fails:
            L.append("- **%s** `%s`: %s" % (where(c), c["check"], c["detail"]))
        L.append("")

    for phase, title, blurb in (
        ("setup", "Setup", "Fresh prefix, `wineboot -u`, and the server exiting on its own afterwards."),
        ("serial", "Serial", "Every program for every arch, one at a time; graphics programs once per backend."),
        ("concurrent", "Concurrent", "All archs at once, every program, in one wineserver lifetime (held by a sentinel process)."),
        ("heavy", "Heavy rungs", "Optional real-world programs (LADDER_HEAVY)."),
        ("shutdown", "Shutdown", "`wineserver -w` must return by itself, then `wineserver -k` must leave nothing behind."),
    ):
        phase_checks = [c for c in checks if c["phase"] == phase]
        if not phase_checks:
            continue
        L.append("## %s" % title)
        L.append("")
        L.append(blurb)
        L.append("")
        if all(c["arch"] == "-" for c in phase_checks):
            for c in phase_checks:
                if c["status"] != "INFO":
                    L.append("- %s `%s`: %s" % (c["status"], c["check"], c["detail"]))
        else:
            phase_archs = [a for a in archs + sorted({c["arch"] for c in phase_checks} - set(archs) - {"-"})
                           if any(c["arch"] == a for c in phase_checks)]
            L += matrix(phase_checks, phase, phase_archs)
            extra = [c for c in phase_checks if c["arch"] == "-" and c["status"] != "INFO"]
            if extra:
                L.append("")
                for c in extra:
                    L.append("- %s `%s`: %s" % (c["status"], c["check"], c["detail"]))
        L.append("")

    t = tso_table(checks, archs)
    if t:
        L.append("## Ordering under FEX_HARDWARE_TSO=1 and =0")
        L.append("")
        L.append("x86 builds only (native ARM code is not affected by FEX's TSO setting). Counts are forbidden (or, for "
                 "`sb` and calibration, allowed) outcomes over iterations; every `pass` row must stay at 0.")
        L.append("")
        L += t
        L.append("")

    # What answered: the environment once per arch, then each graphics stack and the ordering calibration.
    infos, seen_env = [], set()
    for c in checks:
        if c["status"] != "INFO" or c["phase"] != "serial":
            continue
        if c["check"].endswith(".env"):
            if c["arch"] in seen_env:
                continue
            seen_env.add(c["arch"])
            infos.append(dict(c, prog="", check="env"))
        elif re.search(r"\.(adapter|adapter0|renderer|device|calibration\.\w+)$", c["check"]):
            infos.append(c)
    if infos:
        L.append("## What answered")
        L.append("")
        for c in infos:
            L.append("- %s `%s`: %s" % (where(c), c["check"], c["detail"]))
        L.append("")

    xf = [c for c in checks if c["status"] == "XFAIL"]
    if xf:
        L.append("## Known failures")
        L.append("")
        L.append("Listed in `tests/ladder/known-failures.txt`; they don't fail the ladder.")
        L.append("")
        for c in xf:
            L.append("- %s `%s`: %s" % (where(c), c["check"], c["detail"]))
        L.append("")
    sk = [c for c in checks if c["status"] == "SKIP"]
    if sk or skipped_runs:
        L.append("## Skipped")
        L.append("")
        for c in sk:
            L.append("- %s `%s`: %s" % (where(c), c["check"], c["detail"]))
        for r in skipped_runs:
            L.append("- %s: %s" % (where(dict(r, backend=r["backend"], tso=r["tso"])), r["status"][len("skipped: "):] or "skipped"))
        L.append("")
    with open(os.path.join(out, "summary.md"), "w") as f:
        f.write("\n".join(L))
    return verdict, n, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--known")
    a = ap.parse_args()
    env, notes, runs, checks, skipped = collect(a.out, a.known)
    verdict, n, fails = write(a.out, env, notes, runs, checks, skipped)
    print("ladder: %s, %d pass, %d fail, %d known, %d skip" % (verdict, n["PASS"], n["FAIL"], n["XFAIL"], n["SKIP"]))
    for c in fails:
        print("  FAIL %s %s: %s" % (where(c), c["check"], c["detail"]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
