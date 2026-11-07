#!/bin/bash
# Change the Wine patch series without touching the tree the build compiles.
#
#   wine/scripts/series-edit.sh start  [<dir>]   wine/patches applied in a scratch
#                                                worktree <dir> (default: work/series-edit),
#                                                checked out at the commit BEFORE the regen
#                                                patch, on branch series-edit
#   ... commit fixes in <dir> (one commit per fix; git -C <dir> commit ...) ...
#   wine/scripts/series-edit.sh finish [<dir>]   the regen patch back on top (the generators
#                                                whose inputs a fix touched re-run into it),
#                                                wine/patches re-exported and checked, <dir>
#                                                removed
#   wine/scripts/series-edit.sh abort  [<dir>]   remove <dir>, keep wine/patches as it was
#
# Then build/mproton-build.sh --check (compiles only what changed, lists every
# error), more fixes the same way, and the full build once at the end.
#
# Why not edit work/mach9/src directly: going to the commit before the regen
# patch and back rewrites configure and config.h.in. Same content, new
# timestamps, so make reruns configure and recompiles all of Wine.
#
# The worktree shares work/mach9/src's object store, so start is quick
# (~15 s) and needs no network once src has the base commit.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)      # wine/
repo=$(cd "$here/.." && pwd)
src=${MPROTON_WORK:-$repo/work/mach9}/src
cmd=${1:-}
dir=${2:-$repo/work/series-edit}
REGEN_SUBJECT='mproton: Regenerate generated files.'
# Same committer as apply.sh, so commit IDs match what apply.sh produces.
export GIT_COMMITTER_NAME=mproton GIT_COMMITTER_EMAIL=mproton@localhost

die() { echo "series-edit: $*" >&2; exit 1; }
base=$(grep -v '^#' "$here/base.ref" | tr -d '[:space:]')
[ -d "$src/.git" ] || die "$src is not a git checkout (run build/mproton-build.sh workspace once)"

remove_worktree() {
    git -C "$src" worktree remove --force "$1" 2>/dev/null || true
    git -C "$src" worktree prune
    git -C "$src" branch -q -D series-edit series-edit-full series-edit-verify 2>/dev/null || true
}

case "$cmd" in
start)
    [ ! -e "$dir" ] || die "$dir exists: finish or abort the edit in progress first"
    git -C "$src" worktree add --quiet --detach "$dir" "$base"
    bash "$here/apply.sh" --branch series-edit-full "$dir" >/dev/null 2>&1 \
        || die "wine/apply.sh failed in $dir; see: git -C $dir status"
    [ "$(git -C "$dir" log -1 --format=%s)" = "$REGEN_SUBJECT" ] \
        || die "the last patch in wine/patches is not '$REGEN_SUBJECT'"
    git -C "$dir" checkout --quiet -B series-edit HEAD~1
    echo "series-edit: $dir is at the commit before the regen patch ($(git -C "$dir" rev-list --count "$base"..HEAD) patches)."
    echo "Commit your fixes there, then: wine/scripts/series-edit.sh finish${2:+ $dir}"
    ;;
finish)
    [ "$(git -C "$dir" branch --show-current 2>/dev/null)" = series-edit ] \
        || die "$dir is not a series-edit worktree on branch series-edit"
    [ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ] \
        || die "$dir has uncommitted changes; commit or drop them"
    regen=$(git -C "$dir" rev-parse series-edit-full)
    old=$(git -C "$dir" rev-parse series-edit-full~1)
    added=$(git -C "$dir" rev-list --count "$old"..HEAD)
    [ "$(git -C "$dir" rev-list --count HEAD.."$old")" -eq 0 ] || echo "series-edit: note: commits were rewritten, not only added"
    # The regen patch goes back on top as it was; then only the generators
    # whose inputs the fixes touched run again (wine/scripts/regenerate.sh
    # runs all of them), and their output is folded into the regen patch.
    changed() { ! git -C "$dir" diff --quiet "$old" HEAD -- "$@"; }
    gens=()
    changed configure.ac aclocal.m4 && gens+=(autoconf)
    changed server/protocol.def && gens+=(make_requests)
    changed '*.spec' && gens+=(make_specfiles)
    changed dlls/winevulkan/make_vulkan && gens+=(make_vulkan)
    git -C "$dir" cherry-pick --allow-empty "$regen" >/dev/null \
        || die "the regen patch doesn't apply on top of your fixes; resolve in $dir (git cherry-pick --continue), then finish again"
    for g in ${gens[@]+"${gens[@]}"}; do
        echo "series-edit: a fix touched an input of $g; running it"
        case "$g" in
            autoconf)
                command -v autoconf >/dev/null || die "autoconf not found (brew install autoconf); $dir is kept"
                (cd "$dir" && autoconf -f && autoheader -f && rm -rf autom4te.cache configure~ include/config.h.in~) ;;
            make_requests|make_specfiles) (cd "$dir" && "tools/$g") ;;
            make_vulkan) (cd "$dir/dlls/winevulkan" && XDG_CACHE_HOME=${XDG_CACHE_HOME:-${TMPDIR:-/tmp}} python3 ./make_vulkan) ;;
        esac
    done
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
        git -C "$dir" add -u
        git -C "$dir" commit -q --amend --no-edit
        echo "series-edit: regen patch updated"
    fi
    rm -f "$here"/patches/*.patch
    git -C "$dir" format-patch -q --zero-commit --no-signature -N -o "$here/patches" "$base..HEAD"
    # Check: wine/patches applied from scratch gives the same tree.
    want=$(git -C "$dir" rev-parse 'HEAD^{tree}')
    git -C "$dir" checkout --quiet --detach "$base"
    bash "$here/apply.sh" --branch series-edit-verify "$dir" >/dev/null 2>&1 || die "re-applying the exported wine/patches failed"
    got=$(git -C "$dir" rev-parse 'HEAD^{tree}')
    [ "$got" = "$want" ] || die "exported wine/patches give tree $got, the edit had $want; $dir is kept"
    n=$(ls "$here"/patches/*.patch | wc -l | tr -d ' ')
    remove_worktree "$dir"
    echo "series-edit: wine/patches re-exported: $n patches ($added new), tree ${want:0:12} checked."
    echo "Next: build/mproton-build.sh --check   (git -C $repo status wine/patches shows the change)"
    ;;
abort)
    remove_worktree "$dir"
    echo "series-edit: removed $dir; wine/patches unchanged"
    ;;
*)
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
