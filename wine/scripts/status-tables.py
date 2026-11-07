#!/usr/bin/env python3
"""Print the per-patch status tables of docs/wine-tree.md from wine/series.tsv.

usage: status-tables.py wine/series.tsv > fragment.md
"""
import collections, sys

rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
COLS = ["order", "commit", "status", "pass", "group", "author", "subject", "note"]
rows = [dict(zip(COLS, r)) for r in rows]
URL = "https://github.com/ValveSoftware/wine/commit/"
ORDER = ["applied", "hand-merged", "dropped-upstream", "dropped-reverted", "skipped-linux",
         "skipped-generated", "conflict"]
TITLE = {
    "applied": "Applied cleanly",
    "hand-merged": "Applied with a hand merge",
    "dropped-upstream": "Dropped: already upstream (or superseded) in wine-11.20",
    "dropped-reverted": "Dropped: reverted later in Proton's own history",
    "skipped-linux": "Skipped: Linux-only",
    "skipped-generated": "Skipped: deletes generated files (regenerated instead)",
    "conflict": "Not carried: conflicts with the wine-11.20 / athei base",
}

def esc(s):
    return s.replace("|", "\\|").replace("<", "&lt;").replace(">", "&gt;")

cnt = collections.Counter(r["status"] for r in rows)
print("| status | commits |\n|---|---:|")
for st in ORDER:
    print(f"| {st} | {cnt.get(st, 0)} |")
print(f"| **total** | **{len(rows)}** |\n")

groups = collections.OrderedDict()
for r in rows:
    groups.setdefault(r["group"], collections.Counter())[r["status"]] += 1
print("| group | " + " | ".join(ORDER) + " |")
print("|---|" + "---:|" * len(ORDER))
for g, c in sorted(groups.items(), key=lambda kv: -sum(kv[1].values())):
    print(f"| {esc(g)} | " + " | ".join(str(c.get(st, 0) or "") for st in ORDER) + " |")
print()

for st in ORDER:
    sel = [r for r in rows if r["status"] == st]
    if not sel:
        continue
    if st in ("applied", "hand-merged"):
        sel.sort(key=lambda r: int(r["order"] or 0))
    print(f"<details><summary><b>{TITLE[st]}</b> ({len(sel)})</summary>\n")
    if st in ("applied", "hand-merged"):
        print("| # | Proton commit | group | subject | note |\n|---:|---|---|---|---|")
        for r in sel:
            print(f"| {r['order']} | [`{r['commit']}`]({URL}{r['commit']}) | {esc(r['group'])} | {esc(r['subject'])} | {esc(r['note'])} |")
    else:
        print("| Proton commit | group | subject | note |\n|---|---|---|---|")
        for r in sel:
            print(f"| [`{r['commit']}`]({URL}{r['commit']}) | {esc(r['group'])} | {esc(r['subject'])} | {esc(r['note'])} |")
    print("\n</details>\n")
