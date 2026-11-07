# wine-patches

Material for [docs/wine-patch-triage.md](../docs/wine-patch-triage.md).

- `athei-on-proton-11.0/`: 15 of the 36 commits from `athei/wine` `arm64-patched` (wine-11.20 based), rebased onto
  `ValveSoftware/wine` `proton_11.0` (`e60892cc239`). Patches 0001, 0002, 0003 and 0009 needed hand merges; the doc
  says how. The other 20 need wine-11.20 code, and `f1fe4a2` needs one of them, so they aren't carried. These are Wine patches under LGPL-2.1-or-later,
  and the original authorship is kept in the `From:` lines. Apply them with `git am` on top of `proton_11.0`.
- `triage/`: per-commit classification of Proton's Wine commits (`proton-commits.tsv`, `proton-groups.txt`) and
  the apply results of the athei and MetalSharp stacks on Proton 11, wine-11.0, wine-11.20 and WineHQ master.
  TSV columns: n, commit, result, conflicted files, subject.
- `scripts/`: `reproduce.sh` (runs everything), `try-apply.sh`, `classify-proton.py`.
