#!/bin/bash
# mproton-release.sh <tag> [outdir]: turn the bundle build/mproton-build.sh made
# (work/mach9/dist/wine) into a release anyone can run, without an Apple
# Developer account of their own:
#
#   1. check that wine.app (the loader) is signed with a "Developer ID
#      Application" identity, i.e. the bundle step ran with a Developer ID
#      provisioning profile (LOADER_PROFILE); a development profile only runs
#      on the Macs it lists;
#   2. sign every other Mach-O file in the bundle with that same identity, the
#      hardened runtime and a secure timestamp, as notarization requires
#      (mach9 leaves this to the app that embeds the bundle; we ship the
#      bundle itself). wine.app is left as it is: re-signing it loses its
#      profile;
#   3. send the bundle to Apple's notary service and wait for the verdict;
#   4. check that the bundle still runs (bin/wine --version);
#   5. pack it as <outdir>/mproton-<tag>-macos-arm64.tar.xz, as wine/, with
#      a SHA-256 file and release notes naming the commits it was built from.
#
# It changes the bundle in place (step 2). Nothing is uploaded except the
# notarization submission; publishing the GitHub release is a separate,
# manual step (the notes say how).
#
# Environment:
#   NOTARY_PROFILE  notarytool keychain profile (default mproton-notary),
#                   made once with `xcrun notarytool store-credentials`
#   MPROTON_WORK    workspace (default <repo>/work/mach9)
#   SKIP_NOTARIZE=1 do everything but step 3 (for checking the script; the
#                   result must not be published)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${MPROTON_WORK:-$REPO/work/mach9}
DIST=$WORK/dist/wine
NOTARY_PROFILE=${NOTARY_PROFILE:-mproton-notary}
DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
export DEVELOPER_DIR

die() { echo "Error: $*" >&2; exit 1; }
step() { echo; echo "=== $*"; }

[ $# -ge 1 ] || die "usage: $0 <tag> [outdir]   (e.g. $0 v0.1.0)"
TAG=$1
OUT=${2:-$REPO/work/release/$TAG}
[[ $TAG =~ ^[A-Za-z0-9._-]+$ ]] || die "tag '$TAG' has characters other than letters, digits, . _ -"
[ -x "$DIST/bin/wine" ] || die "no bundle at $DIST; run build/mproton-build.sh first"
LOADER=$DIST/lib/wine/aarch64-unix/wine.app
[ -d "$LOADER" ] || die "no loader app at $LOADER"

step "1. loader signature"
info=$(codesign -dvv "$LOADER" 2>&1) || die "wine.app is not signed"
IDENTITY=$(sed -n 's/^Authority=\(Developer ID Application: .*\)$/\1/p' <<< "$info" | head -n 1)
[ -n "$IDENTITY" ] || die "wine.app is not signed with a Developer ID Application identity:" \
    "$(grep '^Authority=' <<< "$info" | head -n 1)." \
    "Rebuild the bundle with LOADER_PROFILE set to a Developer ID provisioning profile."
grep -q '^CodeDirectory.*flags=.*runtime' <<< "$info" || die "wine.app lacks the hardened runtime"
grep -q '^Timestamp=' <<< "$info" || die "wine.app has no secure timestamp (built with LOADER_TIMESTAMP=0?)"
security find-identity -v -p codesigning | grep -qF "\"$IDENTITY\"" \
    || die "the identity '$IDENTITY' is not in the keychain"
echo "  $IDENTITY"

step "2. sign the other Mach-O files"
# Regular files only (symlinks point at files signed here); wine.app as a whole
# is skipped.
list=$(mktemp)
trap 'rm -f "$list"' EXIT
find "$DIST" -type f ! -path "$LOADER/*" -print0 \
    | xargs -0 file -h --no-pad 2>/dev/null \
    | sed -n 's/^\(.*\): Mach-O.*$/\1/p' | sort -u > "$list"
count=$(wc -l < "$list" | tr -d ' ')
[ "$count" -gt 0 ] || die "found no Mach-O files outside wine.app"
n=0
while IFS= read -r f; do
    n=$((n + 1))
    # The first use of a new key may raise a keychain dialog ("codesign wants
    # to sign using key ..."): choose Always Allow, or every file asks again.
    err=$(codesign --force --options runtime --timestamp --sign "$IDENTITY" "$f" 2>&1) \
        || die "signing $f failed: $err" \
               "(a secure timestamp needs network access; a denied keychain prompt also fails here)"
    printf '\r  %d/%d' "$n" "$count"
done < "$list"
echo
bad=0
while IFS= read -r f; do
    codesign --verify --strict "$f" 2>/dev/null || { echo "  ERROR: $f does not verify"; bad=1; }
done < "$list"
codesign --verify --strict --deep "$LOADER" || bad=1
[ "$bad" -eq 0 ] || die "signatures do not verify (above)"
echo "  $count files signed and verified; wine.app verifies"

step "3. notarize"
if [ "${SKIP_NOTARIZE:-0}" = 1 ]; then
    echo "  SKIPPED (SKIP_NOTARIZE=1): do not publish this build"
else
    xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1 \
        || die "no notarytool credentials named '$NOTARY_PROFILE'; make them once with:" \
               "  xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <your Apple ID> --team-id <team>"
    mkdir -p "$OUT"
    zip=$OUT/notarize-$TAG.zip
    rm -f "$zip"
    ditto -c -k --keepParent "$DIST" "$zip"
    result=$(xcrun notarytool submit "$zip" --keychain-profile "$NOTARY_PROFILE" --wait --output-format json) \
        || true
    rm -f "$zip"
    id=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("id",""))' "$result" 2>/dev/null || true)
    status=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("status",""))' "$result" 2>/dev/null || true)
    echo "  submission $id: $status"
    if [ "$status" != "Accepted" ]; then
        [ -n "$id" ] && xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" "$OUT/notary-log.json" >/dev/null 2>&1 \
            && echo "  Apple's log: $OUT/notary-log.json"
        die "notarization was not accepted: ${result:-no response}"
    fi
fi

step "4. smoke test"
version=$("$DIST/bin/wine" --version 2>/dev/null) || die "bin/wine --version failed after signing"
echo "  $version"

step "5. pack"
mkdir -p "$OUT"
name=mproton-$TAG-macos-arm64
tarball=$OUT/$name.tar.xz
rm -f "$tarball"
tar -C "$(dirname "$DIST")" -cf - "$(basename "$DIST")" | xz -T0 > "$tarball"
(cd "$OUT" && shasum -a 256 "$name.tar.xz" > "$name.tar.xz.sha256")
commit=$(git -C "$REPO" rev-parse HEAD)
dirty=$(git -C "$REPO" status --porcelain --untracked-files=no | head -n 1)
build_info=$(cat "$DIST/share/mproton/BUILD-INFO" 2>/dev/null || true)
cat > "$OUT/notes.md" <<EOF
Experimental build of mproton for Apple Silicon Macs (macOS 26.4 or newer).

- Built from mproton \`$commit\`${dirty:+ (with uncommitted changes!)} and the upstream commits pinned in [\`build/pins.env\`](https://github.com/jesscarter/mproton/blob/$commit/build/pins.env).
- Wine: $version. Signed with "$IDENTITY" and notarized by Apple.
- SHA-256: \`$(cut -d' ' -f1 "$OUT/$name.tar.xz.sha256")\`

Unpack it anywhere and point the launcher or mproton Library at the \`wine\` folder (\`MPROTON_WINE_DIR\`); see [docs/running.md](https://github.com/jesscarter/mproton/blob/$commit/docs/running.md).

Source: this bundle contains Wine and other LGPL software. The corresponding source is this repository at \`$commit\` together with the upstream commits pinned there; \`build/mproton-build.sh\` rebuilds it.
${build_info:+
<details><summary>BUILD-INFO</summary>

\`\`\`
$build_info
\`\`\`
</details>}
EOF
echo "  $tarball ($(du -h "$tarball" | cut -f1))"
echo "  $OUT/notes.md"
echo
echo "To publish (after checking the notes):"
echo "  gh release create $TAG '$tarball' '$tarball.sha256' --draft --title 'mproton $TAG' --notes-file '$OUT/notes.md'"
