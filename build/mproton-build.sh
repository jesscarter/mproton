#!/bin/bash
# shellcheck source-path=SCRIPTDIR
# mproton build orchestrator: from a fresh checkout to a runnable Wine bundle
# on an Apple Silicon Mac, on top of athei's mach9 build scripts.
#
#   build/mproton-build.sh [options] [step...]
#
# Steps, in order (default: all of them):
#   workspace  clone mach9 at the pin into work/mach9/scripts and apply
#              build/mach9-patches/series; check out athei/wine at the pin into
#              work/mach9/src, plus the Proton series when wine/apply.sh exists;
#              make sure llvm-mingw is there. The only step that also runs off
#              macOS (handy to check pins and patches).
#   wine       mach9's bin/build-wine.sh: Mesa/KosmicKrisp, GStreamer, Wine,
#              the Wine SDK, FEX, DXMT, vkd3d-proton.
#   dxvk       graphics/dxvk/build.sh (ARM64X + i386), when it is in the checkout.
#   steam      lsteamclient + steam.exe from steam/, when it is in the checkout.
#   bundle     mach9's bin/bundle-wine.sh with DXVK as an extra Direct3D tree,
#              then lsteamclient/steam.exe installed into the bundle.
#
# Options:
#   --from <step>       run <step> and every step after it
#   --force             ignore the stamps of earlier runs; run every step asked for
#   --clean             also wipe build trees (build-wine.sh --clean, DXVK and
#                       steam trees); implies --force
#   --bundle-arg <arg>  pass <arg> to bundle-wine.sh (repeatable), e.g.
#                       --runtime-only, --keep-symbols, --strict-floor
#   --status            print each step's state and exit
#   --check             fast compile check of a Wine series change: update
#                       work/mach9/src to the series (as the workspace step
#                       does), then `make -k` in the existing Wine build tree,
#                       which compiles only what changed and lists every error.
#                       No 32-bit build, SDK, FEX, DXMT, DXVK or bundle. Use it
#                       after each fix; run the full build once at the end.
#   -h, --help          this text
#
# Environment:
#   LOADER_ADHOC=1      sign the loader ad hoc (needs SIP off and the
#                       amfi_get_out_of_my_way=1 boot-arg; local use only), or
#   LOADER_PROFILE=<p>  the .provisionprofile with cross-architecture-support,
#                       signed with your Developer ID (one of the two is required
#                       for the bundle step)
#   MPROTON_WORK        workspace (default: <repo>/work/mach9)
#   MINGW_DIR           llvm-mingw (default: <repo>/toolchains/llvm-mingw, which
#                       scripts/setup.sh fetches when missing)
#   DEVELOPER_DIR       Xcode (default: /Applications/Xcode.app/Contents/Developer)
#   MPROTON_BUNDLE_DEST bundle parent dir (default: <work>/dist; bundle in .../wine)
#   MPROTON_SKIP_DXVK=1, MPROTON_SKIP_STEAM=1   leave a component out
#   MPROTON_CCACHE=0    don't compile through ccache (default: on when installed;
#                       CCACHE_MAXSIZE defaults to 30G)
#
# Every step is idempotent: a step that finished leaves a stamp in
# <work>/.mproton/stamps with the inputs it ran with, and is skipped while
# they are the same. An interrupted step has no stamp and runs again;
# mach9's own scripts then continue incrementally. Full log of each run:
# <work>/.mproton/logs/. See docs/building.md.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$REPO/build/lib/common.sh"
# shellcheck source=pins.env
. "$REPO/build/pins.env"

ALL_STEPS="workspace wine dxvk steam bundle"

usage() { sed -n '3,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

FORCE=0
CLEAN=0
STATUS=0
CHECK=0
FROM=""
STEPS=()
BUNDLE_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --from) [ $# -ge 2 ] || usage 2; FROM="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --clean) CLEAN=1; FORCE=1; shift ;;
        --bundle-arg) [ $# -ge 2 ] || usage 2; BUNDLE_ARGS+=("$2"); shift 2 ;;
        --status) STATUS=1; shift ;;
        --check) CHECK=1; shift ;;
        -h|--help) usage 0 ;;
        -*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
        *) STEPS+=("$1"); shift ;;
    esac
done
for s in ${STEPS[@]+"${STEPS[@]}"} $FROM; do
    in_steps=0
    for a in $ALL_STEPS; do [ "$a" = "$s" ] && in_steps=1; done
    [ "$in_steps" -eq 1 ] || { echo "unknown step: $s (steps: $ALL_STEPS)" >&2; exit 2; }
done
if [ -n "$FROM" ]; then
    [ ${#STEPS[@]} -eq 0 ] || { echo "--from and explicit steps exclude each other" >&2; exit 2; }
    on=0
    for a in $ALL_STEPS; do
        [ "$a" = "$FROM" ] && on=1
        [ "$on" -eq 1 ] && STEPS+=("$a")
    done
elif [ ${#STEPS[@]} -eq 0 ]; then
    for a in $ALL_STEPS; do STEPS+=("$a"); done
fi

# ── Layout ──────────────────────────────────────────────────────────────
# ROOT is mach9's root: profile.sh takes the parent of scripts/ as ROOT, so
# its trees land in ROOT/build/<component>, ROOT/src-fex, ROOT/dist, ...
ROOT="${MPROTON_WORK:-$REPO/work/mach9}"
mkdir -p "$ROOT"
ROOT="$(cd "$ROOT" && pwd)"
SCRIPTS="$ROOT/scripts"
SRC="$ROOT/src"
WINE_BUILD="$ROOT/build/wine"          # mach9's BUILD_DIR
STATE="$ROOT/.mproton"
STAMP_DIR="$STATE/stamps"
BACKUP_DIR="$STATE/backups"
SHIM_DIR="$STATE/shims"
DXVK_TOP="$ROOT/build/mproton-dxvk"
STEAM_O="$ROOT/build/mproton-steam"
BUNDLE_PARENT="${MPROTON_BUNDLE_DEST:-$ROOT/dist}"
DIST="$BUNDLE_PARENT/wine"
DEFAULT_MINGW="$REPO/toolchains/llvm-mingw"
MINGW_DIR="${MINGW_DIR:-$DEFAULT_MINGW}"
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app/Contents/Developer ]; then
    DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
export MINGW_DIR
[ -z "${DEVELOPER_DIR:-}" ] || export DEVELOPER_DIR
# ccache, on by default when installed: mach9 runs Wine's, FEX's, Mesa's and
# GStreamer's compilers through WINE_BUILD_CCACHE. It caches by content, so
# when make rebuilds files only because their timestamps moved, the objects
# come out of the cache instead of the compiler.
if [ "${MPROTON_CCACHE:-1}" != 0 ] && [ -z "${WINE_BUILD_CCACHE:-}" ] && command -v ccache >/dev/null 2>&1; then
    WINE_BUILD_CCACHE=$(command -v ccache)
fi
if [ -n "${WINE_BUILD_CCACHE:-}" ]; then
    export WINE_BUILD_CCACHE
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-30G}"
fi
DEPS_PREFIX=/opt/homebrew
WINE_SERIES="$REPO/wine/apply.sh"

ncpu() { sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4; }

# ── Keys: what each step's result depends on ────────────────────────────
mach9_key() {
    local p patches=()
    while IFS= read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        patches+=("$REPO/$p")
    done < "$REPO/build/mach9-patches/series"
    echo "mach9=$MACH9_COMMIT patches=$(hash_files "$REPO/build/mach9-patches/series" ${patches[@]+"${patches[@]}"})" \
         "fex=$FEX_COMMIT mesa=$MESA_COMMIT dxmt=$DXMT_COMMIT vkd3d-proton=$VKD3D_PROTON_COMMIT"
}
wine_base() {
    local ref
    if [ -f "$REPO/wine/base.ref" ]; then
        ref=$(tr -d '[:space:]' < "$REPO/wine/base.ref")
        if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then echo "$ref"; return; fi
        GIT_TERMINAL_PROMPT=0 git ls-remote "$WINE_REPO" "$ref" | awk 'NR==1 {print $1}'
    else
        echo "$WINE_COMMIT"
    fi
}
wine_src_key() {
    if [ -f "$WINE_SERIES" ]; then
        echo "base=$(wine_base) series=$(hash_tree "$REPO/wine")"
    else
        echo "base=$WINE_COMMIT plain"
    fi
}
src_state() { # HEAD of src, with -dirty when tracked files differ
    [ -d "$SRC/.git" ] || { echo none; return; }
    local h; h=$(git -C "$SRC" rev-parse HEAD)
    if git_dirty "$SRC"; then h="$h-dirty"; fi
    echo "$h"
}
stamp_of() { cat "$(stamp_file "$1")" 2>/dev/null || echo none; }
mingw_version() { "$MINGW_DIR/bin/clang" --version 2>/dev/null | head -n 1; }
wine_key() {
    echo "scripts=$(stamp_of mach9 | sha256_stdin) src=$(src_state) mingw=$(mingw_version)" \
         "xcode=${DEVELOPER_DIR:-none}"
}
dxvk_inputs() {
    hash_files "$REPO/graphics/dxvk/build.sh" "$REPO/graphics/dxvk/dxvk.pin" "$REPO/graphics/dxvk/dxvk.conf" \
        "$REPO"/graphics/dxvk/cross/* "$REPO"/graphics/dxvk/patches/*
}
dxvk_key() { echo "dxvk=$(dxvk_inputs) mingw=$(mingw_version)"; }
macos_floor() { sed -n 's/^MACOS_FLOOR=//p' "$SCRIPTS/bin/lib/profile.sh" 2>/dev/null | head -n 1; }
steam_key() { echo "steam=$(hash_tree "$REPO/steam") wine=$(stamp_of wine | sha256_stdin) floor=$(macos_floor)"; }
signing_desc() {
    if [ "${LOADER_ADHOC:-0}" = 1 ]; then
        echo "adhoc"
    else
        echo "profile=$(hash_files "${LOADER_PROFILE:-/nonexistent}")"
    fi
}
bundle_key() {
    echo "wine=$(stamp_of wine | sha256_stdin) dxvk=$(stamp_of dxvk | sha256_stdin)" \
         "steam=$(stamp_of steam | sha256_stdin) signing=$(signing_desc)" \
         "args=${BUNDLE_ARGS[*]+${BUNDLE_ARGS[*]}} dest=$DIST self=$(hash_files "$0" "$REPO/build/lib/common.sh")"
}

# ── --status ────────────────────────────────────────────────────────────
if [ "$STATUS" -eq 1 ]; then
    echo "workspace: $ROOT"
    for s in mach9 wine-src wine dxvk steam bundle; do
        if [ ! -f "$(stamp_file "$s")" ]; then
            printf '  %-9s not done\n' "$s"
            continue
        fi
        case "$s" in
            mach9) want=$(mach9_key) ;;
            wine-src) want=$(wine_src_key) ;;
            wine) want=$(wine_key) ;;
            dxvk) want=$(dxvk_key) ;;
            steam) want=$(steam_key) ;;
            bundle) want=$(bundle_key) ;;
        esac
        if [ "$(stamp_of "$s")" = "$want" ]; then
            printf '  %-9s done, current\n' "$s"
        else
            printf '  %-9s done, but its inputs changed since (will run again)\n' "$s"
        fi
    done
    exit 0
fi

# ── Run bookkeeping: lock and log ───────────────────────────────────────
mkdir -p "$STATE/logs"
if ! mkdir "$STATE/lock" 2>/dev/null; then
    die "another mproton-build.sh holds $STATE/lock" \
        "If no other run is going (e.g. one was killed), remove that directory."
fi
LOG="$STATE/logs/$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
trap 'rmdir "$STATE/lock" 2>/dev/null || true' EXIT
echo "mproton-build: steps: ${STEPS[*]}; workspace $ROOT; log $LOG"

# ═══ Step: workspace ════════════════════════════════════════════════════
ensure_mingw() {
    if [ -x "$MINGW_DIR/bin/arm64ec-w64-mingw32-clang" ]; then
        info "llvm-mingw: $MINGW_DIR ($(mingw_version))"
        return
    fi
    if [ "$MINGW_DIR" != "$DEFAULT_MINGW" ]; then
        die "MINGW_DIR=$MINGW_DIR has no arm64ec-w64-mingw32-clang"
    fi
    info "llvm-mingw missing; running scripts/setup.sh"
    need_tool gh "scripts/setup.sh downloads llvm-mingw with the GitHub CLI: brew install gh && gh auth login"
    "$REPO/scripts/setup.sh"
    [ -x "$MINGW_DIR/bin/arm64ec-w64-mingw32-clang" ] || die "scripts/setup.sh did not install llvm-mingw"
}

workspace_mach9() {
    local key p
    key=$(mach9_key)
    if stamp_ok mach9 "$key" && [ "$(git -C "$SCRIPTS" rev-parse HEAD 2>/dev/null)" = "$MACH9_COMMIT" ]; then
        ok "mach9: $SCRIPTS at ${MACH9_COMMIT:0:12}, patched (up to date)"
        return
    fi
    stamp_clear mach9
    if [ ! -e "$SCRIPTS" ]; then
        info "cloning $MACH9_REPO into $SCRIPTS"
        GIT_TERMINAL_PROMPT=0 git clone --quiet "$MACH9_REPO" "$SCRIPTS"
    elif [ ! -d "$SCRIPTS/.git" ]; then
        die "$SCRIPTS exists but is not a git checkout" "Move it away; this step clones mach9 there."
    fi
    git_fetch_commit "$SCRIPTS" "$MACH9_REPO" "$MACH9_COMMIT"
    git_force_checkout "$SCRIPTS" "$MACH9_COMMIT" scripts
    [ -f "$SCRIPTS/bin/bundle-wine.sh" ] || die "$SCRIPTS at $MACH9_COMMIT has no bin/bundle-wine.sh; is it mach9?"
    while IFS= read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        [ -f "$REPO/$p" ] || die "build/mach9-patches/series names $p, which does not exist"
        if ! git -C "$SCRIPTS" apply --check "$REPO/$p" 2>"$STATE/apply.err"; then
            die "$p does not apply to mach9 $MACH9_COMMIT:" "$(cat "$STATE/apply.err")" \
                "Rebase the patch, or set MACH9_COMMIT in build/pins.env back to the commit it was made for."
        fi
        git -C "$SCRIPTS" apply "$REPO/$p"
        info "applied $p"
    done < "$REPO/build/mach9-patches/series"
    # Branch pins become commit pins (build/pins.env).
    echo "$FEX_COMMIT" > "$SCRIPTS/pins/fex-src.ref"
    echo "$MESA_COMMIT" > "$SCRIPTS/pins/mesa-src.ref"
    echo "$DXMT_COMMIT" > "$SCRIPTS/pins/dxmt-src.ref"
    echo "$VKD3D_PROTON_COMMIT" > "$SCRIPTS/pins/vkd3d-proton-src.ref"
    info "pinned FEX ${FEX_COMMIT:0:12}, Mesa ${MESA_COMMIT:0:12}, DXMT ${DXMT_COMMIT:0:12}, vkd3d-proton ${VKD3D_PROTON_COMMIT:0:12}"
    stamp_set mach9 "$key"
    ok "mach9: $SCRIPTS at ${MACH9_COMMIT:0:12}, patched"
}

# apply_series_incremental <base>: run wine/apply.sh in a scratch worktree of
# src (branch mproton-series), then check src out at the result. Checking out
# base in src and applying there would rewrite every file the series touches,
# configure included, so configure reruns, config.h changes and nearly all of
# Wine recompiles. Going straight from src's current commit to the new one
# rewrites only the files that differ. The series' commit hashes are the same
# everywhere (fixed committer and dates), so an unchanged series is a no-op.
apply_series_incremental() {
    local base="$1" wt="$STATE/wine-series-wt" new saved
    if [ -e "$wt" ]; then
        git -C "$SRC" worktree remove --force "$wt" 2>/dev/null || rm -rf "$wt"
    fi
    git -C "$SRC" worktree prune
    git -C "$SRC" worktree add --quiet --detach "$wt" "$base"
    info "running wine/apply.sh in a scratch worktree ($wt)"
    bash "$WINE_SERIES" --branch mproton-series "$wt" || die "wine/apply.sh failed" \
        "The scratch worktree $wt is left as apply.sh left it; src is untouched. Fix the series and re-run this step."
    new=$(git -C "$wt" rev-parse HEAD)
    git -C "$SRC" worktree remove --force "$wt"
    [ "$new" != "$base" ] || warn "wine/apply.sh changed nothing"
    if git_dirty "$SRC"; then
        saved=$(backup_changes "$SRC" src)
        note "$SRC had local changes; saved them to $saved"
    fi
    info "moving $SRC from $(git -C "$SRC" rev-parse --short HEAD) to ${new:0:12} (only changed files are rewritten)"
    git -C "$SRC" checkout --quiet --force -B mproton "$new"
    applied=1   # local of the caller, workspace_wine_src
}

workspace_wine_src() {
    local key base head applied=
    key=$(wine_src_key)
    if stamp_ok wine-src "$key" && [ "$(src_state)" = "$(stamp_of wine-src.head)" ]; then
        ok "Wine source: $SRC at $(src_state | cut -c1-12) (up to date)"
        return
    fi
    stamp_clear wine-src
    if [ ! -e "$SRC" ]; then
        info "cloning $WINE_REPO into $SRC (blobless; takes a few minutes)"
        GIT_TERMINAL_PROMPT=0 git clone --quiet --filter=blob:none --no-checkout "$WINE_REPO" "$SRC"
    elif [ ! -d "$SRC/.git" ]; then
        die "$SRC exists but is not a git checkout" "Move it away; this step clones athei/wine there."
    fi
    if [ -f "$WINE_SERIES" ]; then
        base=$(wine_base)
        [ -n "$base" ] || die "cannot resolve wine/base.ref in $WINE_REPO"
        info "Proton Wine series found (wine/apply.sh); base $base"
    else
        base="$WINE_COMMIT"
        skip_msg "Proton Wine series" "wine/apply.sh is not in this checkout; building plain athei/wine" \
            "(no Proton patches, so no steamclient->lsteamclient redirect)"
    fi
    git_fetch_commit "$SRC" "$WINE_REPO" "$base"
    if [ -f "$WINE_SERIES" ] && [ -f "$SRC/.git/index" ]; then
        # src is already checked out: build the series elsewhere and move src
        # straight to it, so make stays incremental (see apply_series_incremental).
        apply_series_incremental "$base"
    else
        git_force_checkout "$SRC" "$base" src
    fi
    if [ -f "$WINE_SERIES" ] && [ -z "$applied" ]; then
        # apply.sh's contract: given a clean checkout, it does
        # `git checkout -B mproton <base>` and `git am`s wine/patches/*.patch
        # with a fixed committer, so the result has the same commit hashes
        # everywhere and a re-run resets and reapplies. It refuses a dirty
        # tree, which git_force_checkout above has cleaned (backing up).
        info "running wine/apply.sh $SRC"
        bash "$WINE_SERIES" "$SRC" || die "wine/apply.sh failed" \
            "The source is left as apply.sh left it; fix the series and re-run this step."
        if [ "$(git -C "$SRC" rev-parse HEAD)" = "$base" ] && ! git_dirty "$SRC"; then
            warn "wine/apply.sh changed nothing in $SRC"
        fi
    fi
    head=$(src_state)
    stamp_set wine-src.head "$head"
    stamp_set wine-src "$key"
    ok "Wine source: $SRC at ${head:0:12}"
}

do_workspace() {
    step "workspace: $ROOT"
    need_tool git "Install the Xcode command line tools: xcode-select --install"
    if is_macos; then ensure_mingw; else note "not macOS: llvm-mingw not checked"; fi
    workspace_mach9
    workspace_wine_src
}

# ═══ Step: wine ═════════════════════════════════════════════════════════
check_xcode() {
    [ -n "${DEVELOPER_DIR:-}" ] && [ -d "$DEVELOPER_DIR" ] \
        || die "full Xcode not found (DEVELOPER_DIR=${DEVELOPER_DIR:-unset})" \
               "Install Xcode, or set DEVELOPER_DIR to its Contents/Developer. DXMT needs its Metal compiler."
    xcrun -f metal >/dev/null 2>&1 \
        || die "Xcode at $DEVELOPER_DIR has no Metal toolchain" \
               "Run: xcodebuild -downloadComponent MetalToolchain"
}

check_brew() {
    local f missing=""
    [ -f "$SCRIPTS/pins/brew-formulae" ] || return 0
    while IFS= read -r f; do
        case "$f" in ''|'#'*) continue ;; esac
        [ -d "$DEPS_PREFIX/opt/$f" ] || missing="$missing $f"
    done < "$SCRIPTS/pins/brew-formulae"
    [ -z "$missing" ] || die "Homebrew formulae missing from $DEPS_PREFIX:$missing" \
        "Run: brew install$missing"
}

# Wine's make reruns configure when src/configure is newer than
# config.status, and regenerates config.h when src/include/config.h.in is
# newer than include/stamp-h. build-wine.sh then has to re-patch config.h,
# which every file includes: a full rebuild. git rewrites those files
# whenever a checkout passes through a commit where they differ (e.g. going
# to the commit before the regen patch and back), so they can be newer with
# the same content. GEN_FILES' blob hashes are recorded after each Wine
# build (record_generated); before the next one, a file whose content is
# unchanged gets the timestamp of its make target back (restore_generated_mtimes).
GEN_FILES="configure include/config.h.in"
gen_hashes() { (cd "$SRC" && git hash-object $GEN_FILES | paste -sd ' ' -); }
record_generated() {
    stamp_set wine-gen "$(gen_hashes)"
    stamp_set wine-built.head "$(src_state)"
}
restore_generated_mtimes() {
    local recorded current i f ref
    [ -f "$WINE_BUILD/config.status" ] && [ -f "$WINE_BUILD/include/stamp-h" ] || return 0
    recorded=($(stamp_of wine-gen)); current=($(gen_hashes))
    i=0
    for f in $GEN_FILES; do
        case "$f" in configure) ref="$WINE_BUILD/config.status" ;; *) ref="$WINE_BUILD/include/stamp-h" ;; esac
        if [ "${recorded[$i]:-}" = "${current[$i]}" ] && [ "$SRC/$f" -nt "$ref" ]; then
            touch -r "$ref" "$SRC/$f"
            note "$f is unchanged since the last Wine build; reset its timestamp so make doesn't rerun configure"
        fi
        i=$((i + 1))
    done
}

do_wine() {
    local key
    step "wine: mach9 build-wine.sh"
    require_macos wine
    [ -f "$(stamp_file mach9)" ] && [ -f "$(stamp_file wine-src)" ] \
        || die "the workspace step has not finished" "Run: build/mproton-build.sh workspace"
    ensure_mingw
    check_xcode
    check_brew
    key=$(wine_key)
    if stamp_ok wine "$key"; then
        ok "Wine stack built from these sources already (--force to run build-wine.sh anyway)"
        return
    fi
    stamp_clear wine
    local args=()
    [ "$CLEAN" -eq 0 ] || args+=(--clean)
    info "MINGW_DIR=$MINGW_DIR DEVELOPER_DIR=$DEVELOPER_DIR"
    info "This is the long one: about 25 minutes on an M2 from scratch."
    restore_generated_mtimes
    "$SCRIPTS/bin/build-wine.sh" ${args[@]+"${args[@]}"}
    [ -f "$ROOT/build/sdk/WINE_COMMIT" ] || die "build-wine.sh finished without a Wine SDK in $ROOT/build/sdk"
    record_generated
    stamp_set wine "$key"
    ok "Wine stack built; SDK in $ROOT/build/sdk ($(cat "$ROOT/build/sdk/WINE_COMMIT"))"
}

# ═══ Step: dxvk ═════════════════════════════════════════════════════════
DXVK_DIST=""
DXVK_STATE="$STATE/dxvk-dist"
dxvk_present() { [ -f "$REPO/graphics/dxvk/build.sh" ]; }

do_dxvk() {
    local key log pin_commit
    step "dxvk: graphics/dxvk/build.sh (ARM64X + i386)"
    if [ "${MPROTON_SKIP_DXVK:-0}" = 1 ]; then
        skip_msg DXVK "MPROTON_SKIP_DXVK=1"; return
    fi
    if ! dxvk_present; then
        skip_msg DXVK "graphics/dxvk/build.sh is not in this checkout (jesscarter/mproton#4 not merged?)"
        return
    fi
    is_macos && ensure_mingw
    shim_sha256sum
    key=$(dxvk_key)
    if stamp_ok dxvk "$key" && [ -f "$(cat "$DXVK_STATE" 2>/dev/null)/COMMIT" ]; then
        DXVK_DIST=$(cat "$DXVK_STATE")
        ok "DXVK staged already: $DXVK_DIST"
        return
    fi
    stamp_clear dxvk
    local args=(arm64x i386)
    [ "$CLEAN" -eq 0 ] || args=(--clean arm64x i386)
    log="$STATE/dxvk-build.log"
    LLVM_MINGW="$MINGW_DIR" DXVK_WORK="$DXVK_TOP/work" DXVK_OUT="$DXVK_TOP/out" \
        "$REPO/graphics/dxvk/build.sh" "${args[@]}" | tee "$log"
    DXVK_DIST=$(sed -n 's/^==> staged //p' "$log" | tail -n 1)
    [ -n "$DXVK_DIST" ] && [ -f "$DXVK_DIST/COMMIT" ] || die "graphics/dxvk/build.sh did not say where it staged DXVK"
    pin_commit=$(sed -n 's/^DXVK_COMMIT=//p' "$REPO/graphics/dxvk/dxvk.pin")
    [ "$(cat "$DXVK_DIST/COMMIT")" = "$pin_commit" ] \
        || die "$DXVK_DIST is DXVK $(cat "$DXVK_DIST/COMMIT"), not the pinned $pin_commit"
    echo "$DXVK_DIST" > "$DXVK_STATE"
    stamp_set dxvk "$key"
    ok "DXVK $(cat "$DXVK_DIST/VERSION") staged in $DXVK_DIST"
}

# DXVK_DIST for the bundle when this run did not build it: what an earlier
# run staged, if the dxvk step is still current (whatever --force says).
load_dxvk_dist() {
    [ -z "$DXVK_DIST" ] || return 0
    [ "${MPROTON_SKIP_DXVK:-0}" != 1 ] && dxvk_present || return 0
    if stamp_current dxvk "$(dxvk_key)" && [ -f "$(cat "$DXVK_STATE" 2>/dev/null)/COMMIT" ]; then
        DXVK_DIST=$(cat "$DXVK_STATE")
    fi
}

# ═══ Step: steam ════════════════════════════════════════════════════════
steam_present() { [ -f "$REPO/steam/Makefile" ]; }
steam_make() { # <make args>...
    PATH="$PATH:$MINGW_DIR/bin" make -C "$REPO/steam" \
        WINE_SRC="$SRC" WINE_OBJ="$WINE_BUILD" O="$STEAM_O" \
        UNIX_ARCHFLAGS="-arch arm64 -mmacosx-version-min=$(macos_floor)" "$@"
}
STEAM_OUTPUTS="aarch64-windows/lsteamclient.dll aarch64-windows/steam.exe i386-windows/lsteamclient.dll i386-windows/steam.exe aarch64-unix/lsteamclient.so"

do_steam() {
    local key f
    step "steam: lsteamclient + steam.exe"
    if [ "${MPROTON_SKIP_STEAM:-0}" = 1 ]; then
        skip_msg "lsteamclient/steam.exe" "MPROTON_SKIP_STEAM=1"; return
    fi
    if ! steam_present; then
        skip_msg "lsteamclient/steam.exe" "steam/Makefile is not in this checkout (jesscarter/mproton#5 not merged?)"
        return
    fi
    require_macos steam
    [ -x "$WINE_BUILD/tools/winegcc/winegcc" ] && [ -f "$WINE_BUILD/dlls/ntdll/ntdll.so" ] \
        || die "no built Wine tree at $WINE_BUILD" "Run: build/mproton-build.sh wine"
    key=$(steam_key)
    if stamp_ok steam "$key"; then
        ok "lsteamclient and steam.exe built already in $STEAM_O"
        return
    fi
    stamp_clear steam
    [ "$CLEAN" -eq 0 ] || steam_make clean
    steam_make -j"$(ncpu)" all
    for f in $STEAM_OUTPUTS; do
        [ -f "$STEAM_O/$f" ] || die "steam/Makefile did not build $STEAM_O/$f"
    done
    "$MINGW_DIR/bin/llvm-readobj" --coff-load-config "$STEAM_O/aarch64-windows/lsteamclient.dll" \
        | grep -qx 'Format: COFF-ARM64X' || die "$STEAM_O/aarch64-windows/lsteamclient.dll is not ARM64X"
    if [ ! -f "$WINE_SERIES" ]; then
        warn "lsteamclient is built, but this Wine has no Proton series: games will not reach it"
    fi
    stamp_set steam "$key"
    ok "lsteamclient + steam.exe built in $STEAM_O"
}

# ═══ Step: bundle ═══════════════════════════════════════════════════════
check_signing() {
    if [ "${LOADER_ADHOC:-0}" = 1 ]; then
        warn "LOADER_ADHOC=1: the loader is signed ad hoc. It only runs with SIP disabled and"
        warn "the amfi_get_out_of_my_way=1 boot-arg (docs/building.md). Never distribute this bundle."
        export LOADER_ADHOC
        return
    fi
    [ -n "${LOADER_PROFILE:-}" ] \
        || die "no signing set up for the loader" \
               "Set LOADER_PROFILE=/path/to/your.provisionprofile (with cross-architecture-support," \
               "and its Developer ID certificate in the keychain), or LOADER_ADHOC=1 for a local" \
               "ad-hoc build. mach9's own signing/loader.provisionprofile is athei's team and won't sign."
    [ -f "$LOADER_PROFILE" ] || die "LOADER_PROFILE=$LOADER_PROFILE does not exist"
    export LOADER_PROFILE
}

# install_steam_into_bundle: lsteamclient and steam.exe next to Wine's own
# builtins, where Proton puts them and the launcher looks: lib/wine/
# {aarch64,i386}-windows and lib/wine/aarch64-unix. wineboot then creates
# their placeholders in system32/syswow64 like for every builtin.
install_steam_into_bundle() {
    local f bad=0 refs
    steam_make install DESTDIR="$DIST" >/dev/null
    for f in $STEAM_OUTPUTS; do
        if ! cmp -s "$STEAM_O/$f" "$DIST/lib/wine/$f"; then
            echo "  ERROR: lib/wine/$f is not installed"; bad=1
        fi
    done
    # The unix side must load from the bundle alone: no Homebrew, /usr/local
    # or build-tree paths (ntdll.so is found through @rpath).
    refs=$(otool -L "$DIST/lib/wine/aarch64-unix/lsteamclient.so" | tail -n +2)
    if grep -E "/opt/homebrew/|/usr/local/|$ROOT" <<< "$refs"; then
        echo "  ERROR: lsteamclient.so links outside the bundle (above)"; bad=1
    fi
    for f in aarch64-windows/lsteamclient.dll i386-windows/lsteamclient.dll aarch64-windows/steam.exe; do
        head -c 128 "$DIST/lib/wine/$f" | grep -qa "Wine builtin DLL" \
            || { echo "  ERROR: lib/wine/$f is not marked as a Wine builtin"; bad=1; }
    done
    [ "$bad" -eq 0 ] || die "lsteamclient/steam.exe are not installed correctly in $DIST"
    ok "lsteamclient.dll (ARM64X, i386), lsteamclient.so and steam.exe installed into lib/wine"
}

check_dxvk_trees() {
    local f bad=0
    for f in dxgi/dxvk/aarch64-windows/dxgi.dll dxgi/dxvk/aarch64-windows/d3d11.dll \
             dxgi/dxvk/aarch64-windows/d3d10core.dll dxgi/dxvk/aarch64-windows/d3d12.dll \
             dxgi/dxvk/i386-windows/dxgi.dll dxgi/dxvk/i386-windows/d3d11.dll \
             d3d9/dxvk/aarch64-windows/d3d9.dll d3d9/dxvk/i386-windows/d3d9.dll; do
        [ -f "$DIST/lib/wine/$f" ] || { echo "  ERROR: no lib/wine/$f"; bad=1; }
    done
    [ "$bad" -eq 0 ] || die "the bundle has no complete DXVK tree although DXVK_DIST was set" \
        "Is build/mach9-patches/0003-dxvk-backend-trees.patch applied in $SCRIPTS?"
    ok "DXVK trees: lib/wine/dxgi/dxvk, lib/wine/d3d9/dxvk (select with WINE_COMPATDB='dxgi=dxvk;d3d9=dxvk')"
}

write_build_info() {
    local out="$DIST/share/mproton/BUILD-INFO"
    mkdir -p "$(dirname "$out")"
    {
        echo "mproton $(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)$(git_dirty "$REPO" && echo -dirty)"
        echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(sw_vers -productVersion 2>/dev/null || uname -r)"
        echo "mach9 $MACH9_COMMIT + $(grep -cvE '^(#|$)' "$REPO/build/mach9-patches/series") patches"
        echo "wine $(src_state)$([ -f "$WINE_SERIES" ] && echo " (Proton series on $(wine_base))" || echo " (plain athei)")"
        echo "fex $FEX_COMMIT"
        echo "mesa $MESA_COMMIT"
        echo "dxmt $DXMT_COMMIT"
        echo "vkd3d-proton $VKD3D_PROTON_COMMIT"
        if [ -n "$DXVK_DIST" ]; then echo "dxvk $(cat "$DXVK_DIST/VERSION") $(cat "$DXVK_DIST/COMMIT")"; else echo "dxvk none"; fi
        if steam_present && [ -f "$(stamp_file steam)" ]; then echo "lsteamclient yes"; else echo "lsteamclient none"; fi
        echo "signing $(signing_desc | cut -c1-20)"
    } > "$out"
    info "build record: ${out#"$DIST"/}"
}

do_bundle() {
    local key steam_built=0
    step "bundle: mach9 bundle-wine.sh + mproton components"
    require_macos bundle
    [ -f "$(stamp_file wine)" ] || die "the wine step has not finished" "Run: build/mproton-build.sh wine"
    need_tool cargo "compatdb.so is Rust: install rustup (https://rustup.rs) and a stable toolchain"
    ensure_mingw
    check_xcode
    check_signing
    load_dxvk_dist
    if [ -n "$DXVK_DIST" ]; then
        info "DXVK: $DXVK_DIST"
    elif [ "${MPROTON_SKIP_DXVK:-0}" != 1 ] && dxvk_present; then
        die "DXVK is in the checkout but not built" "Run: build/mproton-build.sh dxvk (or set MPROTON_SKIP_DXVK=1)"
    else
        skip_msg "DXVK tree" "no DXVK was built; the bundle has DXMT/wined3d/mtld3d only"
    fi
    if [ "${MPROTON_SKIP_STEAM:-0}" != 1 ] && steam_present; then
        [ -f "$(stamp_file steam)" ] || die "steam/ is in the checkout but not built" \
            "Run: build/mproton-build.sh steam (or set MPROTON_SKIP_STEAM=1)"
        steam_built=1
    fi
    key=$(bundle_key)
    if stamp_ok bundle "$key" && [ -e "$DIST/bin/wine" ]; then
        ok "bundle $DIST is current (--force to rebuild it)"
        return
    fi
    stamp_clear bundle
    local args=(--yes)
    [ "$DIST" = "$ROOT/dist/wine" ] || args+=(--dest "$BUNDLE_PARENT")
    args+=(${BUNDLE_ARGS[@]+"${BUNDLE_ARGS[@]}"})
    # bundle-wine.sh deletes and rebuilds the whole bundle every time, so
    # everything below runs again after it.
    DXVK_DIST="$DXVK_DIST" "$SCRIPTS/bin/bundle-wine.sh" "${args[@]}"
    [ -e "$DIST/bin/wine" ] || die "bundle-wine.sh finished without $DIST/bin/wine"
    step "bundle: mproton components"
    if [ -n "$DXVK_DIST" ]; then check_dxvk_trees; fi
    if [ "$steam_built" -eq 1 ]; then install_steam_into_bundle; fi
    write_build_info
    stamp_set bundle "$key"
    ok "bundle ready: $DIST"
}

# ═══ --check ═════════════════════════════════════════════════════════════
do_check() {
    local last path n
    step "check: make -k in $WINE_BUILD"
    require_macos check
    [ -f "$WINE_BUILD/Makefile" ] && [ -f "$WINE_BUILD/config.status" ] \
        || die "no configured Wine build in $WINE_BUILD" "Run the full build once: build/mproton-build.sh"
    last=$(stamp_of wine-built.head)
    if [ "$last" != none ] && ! git -C "$SRC" diff --quiet "${last%-dirty}" -- $GEN_FILES; then
        die "configure or config.h.in changed since the last Wine build" \
            "--check would leave config.h unpatched; run the full build: build/mproton-build.sh --from wine"
    fi
    restore_generated_mtimes
    path="$MINGW_DIR/bin:$PATH"
    [ ! -d "$WINE_BUILD/ccache-bin" ] || [ -z "${WINE_BUILD_CCACHE:-}" ] || path="$WINE_BUILD/ccache-bin:$path"
    n=$(sysctl -n hw.ncpu)
    if PATH="$path" make -C "$WINE_BUILD" -k -j"$n" >"$STATE/check.log" 2>&1; then
        ok "Wine compiles (only the 64-bit tree; log $STATE/check.log)"
    else
        grep -E "error:|\*\*\* \[" "$STATE/check.log" | sort -u | head -60 || true
        die "the Wine build has errors (all of them are listed above; log $STATE/check.log)" \
            "Fix them all, re-run --check, then run the full build."
    fi
}

# ── Run ─────────────────────────────────────────────────────────────────
if [ "$CHECK" -eq 1 ]; then
    workspace_wine_src
    do_check
    exit 0
fi
for s in "${STEPS[@]}"; do
    "do_$s"
done

step "summary"
for s in "${STEPS[@]}"; do info "$s: done"; done
if [ ${#SKIPPED[@]} -gt 0 ]; then
    for s in "${SKIPPED[@]}"; do warn "skipped $s"; done
fi
if [ -e "$DIST/bin/wine" ]; then
    info "bundle: $DIST"
    info "try:    WINEPREFIX=\$HOME/mproton-pfx $DIST/bin/wine winecfg"
fi
info "log:    $LOG"
