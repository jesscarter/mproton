#!/bin/bash
# Build the mproton Wine tree: athei's arm64-patched at the commit in wine/base.ref,
# plus the patch series in wine/patches, as commits on branch `mproton`.
#
# usage: wine/apply.sh [--branch <name>] <dest>
#
#   <dest> missing or empty: athei/wine is cloned there.
#   <dest> an existing git checkout (e.g. a mach9 checkout's src/, which is a clone of
#          athei/wine): it must have no uncommitted changes; the base commit is fetched
#          into it and <name> (default: mproton) is reset to base + series.
#
# The series is applied with `git am --committer-date-is-author-date` and a fixed
# committer, so the resulting commit IDs are the same on every machine.
# The wine-<version> release tag is fetched from the WineHQ GitHub mirror, because
# mach9's package step needs `git describe` to find it.
#
# Environment: ATHEI_WINE_URL (default https://github.com/athei/wine),
#              WINE_MIRROR_URL (default https://github.com/wine-mirror/wine)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
branch=mproton
while [ $# -gt 0 ]; do
    case "$1" in
        --branch) branch=$2; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
dest=${1:?usage: wine/apply.sh [--branch <name>] <dest>}
base=$(grep -v '^#' "$here/base.ref" | tr -d '[:space:]')
url=${ATHEI_WINE_URL:-https://github.com/athei/wine}
mirror=${WINE_MIRROR_URL:-https://github.com/wine-mirror/wine}

if [ ! -e "$dest/.git" ]; then   # -e: a worktree has a .git file
    if [ -e "$dest" ] && [ -n "$(ls -A "$dest")" ]; then
        echo "error: $dest exists, is not empty and is not a git checkout" >&2; exit 1
    fi
    git clone --no-checkout --filter=blob:none "$url" "$dest"
    fresh=1
fi
cd "$dest"
if [ -z "${fresh:-}" ] && [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "error: $dest has uncommitted changes" >&2; git status --short --untracked-files=no >&2; exit 1
fi
if [ -d .git/rebase-apply ]; then
    echo "error: a git am/rebase is in progress in $dest" >&2; exit 1
fi
git cat-file -e "$base^{commit}" 2>/dev/null || git fetch --quiet "$url" "$base"
git checkout --quiet -B "$branch" "$base"

version=$(sed -n 's/^Wine version //p' VERSION)
git rev-parse -q --verify "refs/tags/wine-$version" >/dev/null ||
    git fetch --quiet --no-tags "$mirror" "refs/tags/wine-$version:refs/tags/wine-$version" ||
    echo "warning: could not fetch tag wine-$version; git describe will fail" >&2

GIT_COMMITTER_NAME=mproton GIT_COMMITTER_EMAIL=mproton@localhost \
    git -c core.autocrlf=false am --quiet --committer-date-is-author-date --keep-cr "$here"/patches/*.patch
echo "$dest: branch $branch at $(git rev-parse --short HEAD) ($(git describe --long HEAD 2>/dev/null || echo 'no describe')), $(git rev-list --count "$base"..HEAD) patches on $base"
