#!/usr/bin/env python3
"""List lines a commit range adds inside preprocessor branches a Linux build never compiles
(#ifdef __APPLE__, #else of #ifdef __linux__, ...), i.e. lines that need a darwin compile.

usage: darwin-only-hunks.py <wine-src> <base> [<tip>]
"""
import re, subprocess, sys
src, base = sys.argv[1], sys.argv[2]
tip = sys.argv[3] if len(sys.argv) > 3 else "HEAD"
DARWIN = re.compile(r"__APPLE__|__MACH__|__darwin__|HAVE_MACH|TARGET_OS_")
LINUX = re.compile(r"__linux__|linux")

diff = subprocess.run(["git", "-C", src, "diff", "-U0", base, tip, "--", "*.c", "*.h", "*.m"],
                      capture_output=True, text=True, errors="replace").stdout
added = {}
f = None
for l in diff.splitlines():
    if l.startswith("+++ "):
        f = l[6:] if l.startswith("+++ b/") else None
    elif l.startswith("@@") and f:
        m = re.search(r"\+(\d+)(?:,(\d+))?", l)
        start, n = int(m.group(1)), int(m.group(2) or 1)
        added.setdefault(f, set()).update(range(start, start + n))

def darwin_only(stack):
    # stack entries: (condition, in_else)
    for cond, in_else in stack:
        if DARWIN.search(cond) and not in_else and not re.search(r"!\s*defined\s*\(\s*__APPLE__|ifndef\s+__APPLE__", cond):
            return cond
        if LINUX.search(cond) and (in_else != bool(re.search(r"ifndef|!\s*defined", cond))):
            return cond + (" (#else)" if in_else else "")
    return None

for f, lines in sorted(added.items()):
    text = subprocess.run(["git", "-C", src, "show", f"{tip}:{f}"], capture_output=True, text=True, errors="replace").stdout.splitlines()
    stack, hits = [], []
    for i, l in enumerate(text, 1):
        s = l.strip()
        if re.match(r"#\s*if", s): stack.append([s, False])
        elif re.match(r"#\s*elif", s) and stack: stack[-1] = [s, False]
        elif re.match(r"#\s*else", s) and stack: stack[-1][1] = True
        elif re.match(r"#\s*endif", s) and stack: stack.pop()
        elif i in lines:
            c = darwin_only(stack)
            if c: hits.append((i, c, l))
    for i, c, l in hits:
        print(f"{f}:{i}: [{c}] {l.strip()[:100]}")
