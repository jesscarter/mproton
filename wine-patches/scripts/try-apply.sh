#!/bin/bash
# try-apply.sh: cherry-pick a patch stack onto a base, one commit at a time,
# and record which commits apply cleanly, which apply with a 3-way merge,
# and which conflict (and in which files).
#
# usage: try-apply.sh <wine-repo> <base-ref> <stack-base> <stack-tip> <out.tsv> [mode]
#   mode = cumulative (default): keep clean picks, skip conflicting ones
#          isolated: test each commit alone on <base-ref>
#
# Output TSV columns: n, commit, result (clean|conflict|empty), conflicted files, subject
set -u
repo=$1 base=$2 sbase=$3 stip=$4 out=$5 mode=${6:-cumulative}
wt=$(mktemp -d "${TMPDIR:-/tmp}/tryapply.XXXX")
git -C "$repo" worktree add -q --detach "$wt" "$base" || exit 1
cd "$wt"
git config user.email triage@localhost; git config user.name triage
git config rerere.enabled false
: > "$out"
n=0
for c in $(git rev-list --reverse --no-merges "$sbase..$stip"); do
    n=$((n+1))
    subj=$(git log -1 --format=%s "$c")
    [ "$mode" = isolated ] && git reset -q --hard "$base"
    if git cherry-pick -x --allow-empty "$c" >/dev/null 2>&1; then
        if git diff --quiet HEAD~1 HEAD; then r=empty; else r=clean; fi
        files=""
    else
        files=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
        [ -z "$files" ] && files="(no-index: $(git status --porcelain | head -3 | tr '\n' ' '))"
        r=conflict
        git cherry-pick --abort >/dev/null 2>&1 || git reset -q --hard
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$n" "${c:0:11}" "$r" "$files" "$subj" >> "$out"
done
echo "result: $(git rev-parse HEAD)" >&2
[ "$mode" = cumulative ] && git branch -f "tryapply/$(basename "$out" .tsv)" HEAD
cd / && git -C "$repo" worktree remove --force "$wt"
