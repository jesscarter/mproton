# shellcheck shell=bash
# Helpers for build/mproton-build.sh. Sourced, not run. Written for the bash
# macOS ships (3.2): no associative arrays, no mapfile, no ${var,,}, and an
# empty array is expanded as ${a[@]+"${a[@]}"} under set -u.

# ── Output ──────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    C_BOLD=$'\033[1m' C_DIM=$'\033[2m' C_RED=$'\033[31m' C_YEL=$'\033[33m' C_GRN=$'\033[32m' C_OFF=$'\033[0m'
else
    C_BOLD="" C_DIM="" C_RED="" C_YEL="" C_GRN="" C_OFF=""
fi
step()  { printf '\n%s=== %s%s\n' "$C_BOLD" "$*" "$C_OFF"; }
info()  { printf '  %s\n' "$*"; }
note()  { printf '  %s%s%s\n' "$C_DIM" "$*" "$C_OFF"; }
ok()    { printf '  %s%s%s\n' "$C_GRN" "$*" "$C_OFF"; }
warn()  { printf '  %sWARNING:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
# skip_msg <what> <why>...: a component that is not built, and why. Collected
# for the summary at the end, so a missing piece is not lost in the scroll.
SKIPPED=()
skip_msg() {
    local what="$1"; shift
    printf '  %sSKIPPED%s %s: %s\n' "$C_YEL" "$C_OFF" "$what" "$*"
    SKIPPED+=("$what: $*")
}
die() {
    printf '%sError:%s %s\n' "$C_RED" "$C_OFF" "$1" >&2
    shift
    local line
    for line in "$@"; do printf '       %s\n' "$line" >&2; done
    exit 1
}

is_macos() { [ "$(uname -s)" = Darwin ]; }

# require_macos <step>: exit unless this is an arm64 Mac.
require_macos() {
    is_macos || die "step '$1' needs macOS (this is $(uname -s))" \
        "Only the workspace step runs elsewhere; it is useful to check pins and patches."
    [ "$(uname -m)" = arm64 ] || die "step '$1' needs an Apple Silicon Mac (this is $(uname -m))"
}

# ── Hashing ─────────────────────────────────────────────────────────────
# sha256 of stdin, hex only. macOS has shasum; Linux has sha256sum.
sha256_stdin() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | cut -d' ' -f1
    else
        sha256sum | cut -d' ' -f1
    fi
}

# hash_files <file>...: one hash over the names and contents of the files
# that exist. A missing file counts as its name with no content.
hash_files() {
    local f
    for f in "$@"; do
        printf '%s\0' "$f"
        if [ -f "$f" ]; then cat "$f"; fi
        printf '\0'
    done | sha256_stdin
}

# hash_tree <dir>: one hash over every tracked-or-not regular file under
# <dir> (paths relative to it, sorted), excluding .git. Empty when <dir> is
# missing.
hash_tree() {
    [ -d "$1" ] || { echo ""; return 0; }
    (cd "$1" && find . -type f ! -path './.git/*' | LC_ALL=C sort | while IFS= read -r f; do
        printf '%s\0' "$f"; cat "$f"; printf '\0'
    done) | sha256_stdin
}

# ── Stamps ──────────────────────────────────────────────────────────────
# A step that finished writes STAMP_DIR/<step> with the key of what it was
# run with. A later run skips the step when the key is the same, and runs it
# again when anything in the key changed or the stamp is missing (it never
# finished). --force ignores stamps.
stamp_file() { echo "$STAMP_DIR/$1"; }
stamp_current() { # <step> <key>: the step finished with these inputs
    [ -f "$(stamp_file "$1")" ] && [ "$(cat "$(stamp_file "$1")")" = "$2" ]
}
stamp_ok() { # <step> <key>: ... and --force does not ask to run it again
    [ "${FORCE:-0}" -eq 0 ] && stamp_current "$1" "$2"
}
stamp_set() { # <step> <key>
    mkdir -p "$STAMP_DIR"
    printf '%s\n' "$2" > "$(stamp_file "$1")"
}
stamp_clear() { rm -f "$(stamp_file "$1")"; }

# ── Git ─────────────────────────────────────────────────────────────────
# git_dirty <dir>: succeed when tracked files differ from HEAD.
git_dirty() {
    [ -n "$(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null)" ]
}

# backup_changes <dir> <label>: save the uncommitted tracked changes of the
# checkout <dir> as a patch under BACKUP_DIR and print its path. Nothing is
# ever thrown away without this.
backup_changes() {
    local out
    mkdir -p "$BACKUP_DIR"
    out="$BACKUP_DIR/$2-$(date +%Y%m%d-%H%M%S)-$$-$(git -C "$1" rev-parse --short HEAD).diff"
    git -C "$1" diff HEAD > "$out"
    echo "$out"
}

# git_has_commit <dir> <commit>: succeed when <dir> has <commit>.
git_has_commit() {
    git -C "$1" cat-file -e "$2^{commit}" 2>/dev/null
}

# git_fetch_commit <dir> <repo> <commit>: make sure <dir> has <commit>,
# fetching it by hash from <repo> (GitHub serves reachable commits by hash).
git_fetch_commit() {
    git_has_commit "$1" "$3" && return 0
    info "fetching $3 from $2"
    GIT_TERMINAL_PROMPT=0 git -C "$1" fetch --quiet "$2" "$3" \
        || GIT_TERMINAL_PROMPT=0 git -C "$1" fetch --quiet --tags "$2" \
        || die "cannot fetch $3 from $2"
    git_has_commit "$1" "$3" || die "$2 has no commit $3 (check build/pins.env)"
}

# git_force_checkout <dir> <commit> <label>: put <dir> at <commit>, detached,
# with a clean work tree. Uncommitted tracked changes are first saved with
# backup_changes, including changes an earlier run made itself (patches,
# pins): cheap, and never wrong.
git_force_checkout() {
    local dir="$1" commit="$2" label="$3" saved
    # A clone made with --no-checkout has no index yet: nothing to save.
    if [ -f "$dir/.git/index" ] && git_dirty "$dir"; then
        saved=$(backup_changes "$dir" "$label")
        note "$dir had local changes; saved them to $saved"
    fi
    git -C "$dir" -c advice.detachedHead=false checkout --quiet --force --detach "$commit"
}

# ── Tools ───────────────────────────────────────────────────────────────
# need_tool <tool> <hint>: exit unless <tool> is on PATH.
need_tool() {
    command -v "$1" >/dev/null 2>&1 || die "$1 not found" "$2"
}

# shim_sha256sum: graphics/dxvk/build.sh calls sha256sum, which macOS may
# lack (it has shasum). Put a shim for it into SHIM_DIR, ahead on PATH.
shim_sha256sum() {
    command -v sha256sum >/dev/null 2>&1 && return 0
    mkdir -p "$SHIM_DIR"
    cat > "$SHIM_DIR/sha256sum" <<'EOF'
#!/bin/sh
exec shasum -a 256 "$@"
EOF
    chmod +x "$SHIM_DIR/sha256sum"
    PATH="$SHIM_DIR:$PATH"
    export PATH
}
