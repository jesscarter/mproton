# Handoff: porting the rest of Proton's Wine commits

For the next session that continues [docs/wine-tree.md](../docs/wine-tree.md). Read that doc first: it explains the series, the statuses and what still needs a Mac.

## Where things stand

- `main` has the series: 724 patches in `wine/patches/` on athei `arm64-patched` `5959a04b1af` (`wine/base.ref`). That's 720 Proton commits plus 4 mproton patches, and the last patch is always `mproton: Regenerate generated files.`
- `wine/series.tsv` has one row per Proton commit (`ValveSoftware/wine` `proton_11.0` @ `e60892cc239`, 1454 commits on wine-11.0). **159 have status `conflict`** (246 before batch 1, 200 before batch 2). Those are the job.
- A clean aarch64-Linux build of the series has 0 errors. So does athei alone. Keep it at 0.
- **Progress:** batch 1 (high-confidence groups) and batch 2 (d3dx, multimedia) are done; see [Porting the conflicts](../docs/wine-tree.md#porting-the-conflicts). Of those groups, only `347bd52e55f` (test-only) and `60c7e66a137` (needs the OpenGL commit `80eb9f6e2bd`) are left, both with a note. **Next: win32u + wineserver (36), then winebus/xinput (16), then ntdll (52).** Hand-set statuses (superseded, Linux-only, held) go in `wine/scripts/manual-status.tsv`.

## The task

Port the `conflict` commits onto the series, group by group, a build-checked batch at a time. For each one, read Proton's change and the 11.20 code that replaced what it touched, then write the change against the new code. Don't resolve conflict markers blindly. Suggested order (counts are from `wine/series.tsv`, column `group`):

1. ~~**High confidence (~50):** d3dx/d3dcompiler (20), `wine.inf`/prefix (2), networking/winhttp (3), audio (3), Proton integration (1), other PE-side fixes (24: bcrypt, appx package APIs, jscript/mshtml, icu, xaudio prefer-native, …).~~ Done (batch 1).
2. ~~**Multimedia (35):** mf session, the winegstreamer `aac_decoder` → `audio_decoder` rename (`94af7660fbb`, the root of a chain), the Vorbis/Opus decoders, the media-converter demuxer in winedmo, the D3D11 video processor. Media-converter is Proton's video path, so this group matters most.~~ Done (batch 2), along with the d3dx rest.
3. **win32u + wineserver (35):** touch/pointer input, rawinput (`eb0c49cf1ed` adds `SEND_HWMSG_NO_RAW`; then re-add the removed `544a0817198`), layered-window hacks, NtFlushKey, file security xattrs, CPU topology.
4. **winebus/xinput (15):** the xinput refactors (`64ca68c68d6`, …), then re-add the removed `965d4e24cdd`.
5. **ntdll `virtual.c`/`loader.c` (52):** this is athei's W^X / 4K-page / FEX code. Port the simple ones (per-game env switches in `hacks_init()`, heap hacks in PE `loader.c`). For `WINE_SIMULATE_WRITECOPY`, the address-space limits and the GDB link map, port only if it's clearly safe, and otherwise write in the TSV note that it needs a Mac.
6. **Don't port the fullscreen hack (21).** The plan is to leave it off on macOS (winemac scales on its own). Leave those as `conflict` with that reason.

Commit statuses in the TSV are derived from the branch (see below), so a ported commit becomes `hand-merged` automatically.

## Rebuilding the environment (fresh container)

**First check whether the old one survived:** if `/home/user/work/series` exists with branch `mproton/series`, and `/opt/llvm-mingw`, `/opt/sysroot-arm64` and `/opt/xbin` exist, skip this section. In that case, check that `git -C /home/user/work/series rev-parse HEAD^{tree}` equals the tree `wine/apply.sh` produces from `main` (it was `65cfb5187179` after batch 1). `/home/user/work/build-final` is a finished build of exactly that tree, so `make -k` there is incremental.

Otherwise, everything outside the repo is gone. On x86_64 Ubuntu 24.04:

```bash
mkdir -p /home/user/work && cd /home/user/work
# Wine repo with every ref the tools use (about 2 GB)
git clone -q --single-branch -b arm64-patched https://github.com/athei/wine wine
cd wine
git remote add proton https://github.com/ValveSoftware/wine && git fetch -q proton proton_11.0
git branch proton e60892cc2398b0a83c332a2078c7b85e47bdb42b
git remote add winehq https://github.com/wine-mirror/wine
git fetch -q winehq tag wine-11.0 tag wine-11.20 master   # master: to see how upstream later did the same change
git config gc.auto 0
# The series as branch mproton/series in worktree ../series
git worktree add -f ../series 5959a04b1afe06ea1b119fa622f0b29f408ac8c4
cd ../series && git checkout -q -b mproton/series
GIT_COMMITTER_NAME=mproton GIT_COMMITTER_EMAIL=mproton@localhost \
  git am -q --committer-date-is-author-date /home/user/mproton/wine/patches/*.patch
```

Toolchain for the build check (`wine/scripts/linux-build-check.sh` documents the variables):

- **llvm-mingw** at `/opt/llvm-mingw`: download `llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64.tar.xz` from the `mstorsjo/llvm-mingw` GitHub releases, untar it, and symlink the directory there.
- **arm64 sysroot** at `/opt/sysroot-arm64`. `apt-get install ...:arm64` mostly fails here because of multiarch conflicts, so don't fight apt:
  1. Add an arm64 ports source (`http://ports.ubuntu.com/ubuntu-ports/`, noble, noble-updates and noble-security, main and universe, `Architectures: arm64`), and add `Architectures: amd64` to the existing `ubuntu.sources` entries.
  2. Run `dpkg --add-architecture arm64 && apt-get update`.
  3. `apt-get install libc6-dev:arm64 libgcc-13-dev:arm64 linux-libc-dev:arm64 libfreetype-dev:arm64 libgnutls28-dev:arm64 libvulkan-dev:arm64 libudev-dev:arm64 libdbus-1-dev:arm64` works.
  4. For the rest (SDL2, PulseAudio, ALSA, X11 + extensions, EGL/GL, GStreamer, Wayland, libdrm, libusb, CUPS, krb5, pcap), run `apt-get download` on each package plus its `apt-cache depends --recurse` arm64 dependencies, then `dpkg-deb -x` everything into the sysroot.
  5. Copy `/usr/include` and `/usr/lib/aarch64-linux-gnu` into the sysroot with `cp -an`, **and `/usr/lib/gcc/aarch64-linux-gnu`** (crtbeginS.o, libgcc; without it the cross configure fails with "C compiler cannot create executables"). Then make `lib` a symlink to `usr/lib`: some debs (cryptsetup) ship `/lib/...`, so `dpkg-deb -x` creates a real `lib` dir first; merge it into `usr/lib` with `cp -an lib/. usr/lib/` and replace it with the symlink, and check again after extracting more debs.
  6. Since batch 2 the sysroot also has FFmpeg (`libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev` plus recursive deps, same download-and-extract), so winedmo's unix side and the media-converter demuxer are compiled. Downloading ~640 debs one by one takes about 30 minutes. The Wayland debs bring an arm64 `/usr/bin/wayland-scanner` that configure then picks up; install the host's `libwayland-bin` and symlink the sysroot's `usr/bin/wayland-scanner` to `/usr/bin/wayland-scanner`, or winewayland.drv's protocol files fail to generate.
- **`/opt/xbin`**: symlinks `aarch64-linux-gnu-{ar,ranlib,strip,nm,objcopy,objdump}` → `/opt/llvm-mingw/bin/llvm-*`, and `aarch64-linux-gnu-ld` → `/opt/llvm-mingw/bin/ld.lld`. (`binutils-aarch64-linux-gnu` from apt was missing its libbfd here. Don't put `-fuse-ld=lld` in `CC`: it breaks configure's `-Werror` probes.)
- Host packages: `bison flex autoconf pkg-config gettext`, plus host `libfreetype-dev`, which the tools build needs for `sfnt2fon`.

## The workflow that worked

- **Hand-port a commit:** `git cherry-pick -x <proton-sha>` at the tip, then resolve. If it should sit earlier in the series, move it with `git rebase -i` (sequence editor = a small script). Then amend the message to add a trailer: `mproton-resolved: <what changed and why>`. The driver reads that trailer as `hand-merged` plus the note.
- **Fix a commit that's already in the series:** `GIT_SEQUENCE_EDITOR="sed -i 's/^pick <sha> /edit <sha> /'" git rebase -i 5959a04b1af`, edit, then `git commit --amend` adding the trailer, then `git rebase --continue`. Several fixes in one rebase: a script that marks every target `edit` and dispatches on `git log -1 --format=%s` at each stop.
- **Remove a commit:** drop it in a rebase, and add a line to `wine/scripts/manual-status.tsv` with `conflict` and the reason, or `dropped-upstream` if 11.20 already has the change.
- **The regeneration patch stays last:** before rebasing, `git reset --hard HEAD~1` to take it off. After the series changes, run `wine/scripts/regenerate.sh <series>` and commit it again with the same message.
- **Build check:** `JOBS=4 wine/scripts/linux-build-check.sh <src> <build-dir>`. A full build takes 1–2 h on 4 cores. While iterating, reuse the build dir and run `make -k` in `<build>/wine`, with `PATH=/opt/xbin:/opt/llvm-mingw/bin:$PATH`. If `tools/makedep.c` changed, delete `<build>/tools` first.
- **Statuses:** `RESUME=1 NOPASS=1 python3 wine/scripts/build-series.py /home/user/work/wine 5959a04b1af… e60892cc239… wine-11.0 wine-11.20 wine-patches/triage/proton-commits.tsv wine/series.tsv` rewrites the TSV from the branch without touching the worktree. Leave out `NOPASS` to also run a retry pass that cherry-picks whatever now applies (that does modify the worktree, so not during a build).
- **Export:**
  1. `rm -rf wine/patches && git -C <series> format-patch -q --zero-commit --no-signature -N -o wine/patches 5959a04b1af..HEAD`.
  2. Run `wine/apply.sh` into an empty dir and check that the tree hash equals the series branch's.
  3. Regenerate the doc tables with `wine/scripts/status-tables.py wine/series.tsv`, and update the counts in `docs/wine-tree.md`.
  4. Run `wine/scripts/darwin-only-hunks.py <series> 5959a04b1af` and list anything new in the doc.

## Lessons learned

- **Batch 2:** 11.20 moved whole components, so "the file conflicts" often means "the file moved": winegstreamer's video processor is now `dlls/msvproc` (FFmpeg/swscale) and its resampler `dlls/resampledmo` (FFmpeg/swresample). Proton's `video_processor.c`/`resampler.c` changes have to be re-applied there. `git cherry-pick` follows the rename sometimes (it did for a9ee089454d) and not others (delete/modify conflict).
- Rename detection can match the wrong file: qasf's deleted `version.rc` was matched to `dlls/tiptsf/tiptsf.idl`. Check what a conflict path really is before resolving it.
- Several winegstreamer conflicts are context from `80eb9f6e2bd` (the OpenGL WMReader pipeline, OpenGL group): `use_opengl`, `enable_opengl` game lists. Leave those hunks out and say so; port them with that commit.
- Check Proton's code for bugs while porting, not only the merge: the media-converter demuxer double-freed the format context on its default (no fozdb) path, and `wg_task_pool` passed an uninitialized `GError *`.
- Probe a group with a throwaway branch first: cherry-pick each commit in order, abort on conflict, and list the conflicting files. It shows the moved files and the order dependencies before you start.

- Most conflicts come from 11.20 rewriting the code, not from athei. `git show winehq/master:<file>` often shows how upstream later did the same thing. Many Proton commits are cherry-picks of upstream commits (`cherry picked from commit …`).
- Commits that apply cleanly can still fail to build because they depend on a commit that wasn't carried (`get_env`, `window_surface_get`, `SEND_HWMSG_NO_RAW`, the `audio_decoder` rename) or on an 11.20 API change (`struct ratio` DPI, `get_d3dkmt_adapter()`, `create_key_recursive()` taking the name by value). Always build after a batch.
- Watch for duplicates of things 11.20 already has (pdh, `RtlWow64SuspendThread`). Grep 11.20 for the symbol before adding it.
- Use the variable name `is_unix`, not `unix`: `unix` is a predefined macro on Linux.
- 11.20's makedep crashes when a test dir's module is missing from `configure.ac`.
- `WINE_PACKAGE_FLAGS(FOO,...,[-lfoo],...)` leaves `FOO_LIBS=-lfoo` even when the check fails. Code that is now built without the library (the opencl stub) needs it cleared.
- In an incremental build, unixlib objects don't depend on `config.h`. After a configure change, delete stale `.o` files whose `#ifdef HAVE_*` path changed (amd_ags_x64 linked an old libdrm `unixlib.o`).
- For d3dx10/d3dx11, Proton's code builds on its own `struct d3d10_texture` helpers. 11.20 has different upstream helpers (`d3dx_texture_info`, `d3d10_staging_resource_from_d3dx_texture_info()`), so write the d3dx10/11 side on those. The d3dx9_36 helper hunks apply cleanly. Proton's d3dx10 tests use test helpers 11.20 doesn't have; leave them out and say so.
- The steamclient → lsteamclient redirect is only hooked in the x86 signal handlers. Porting it to arm64/FEX is a separate, open item (see `steam/README.md`).

## Done means

Each batch builds with 0 errors from scratch through `wine/apply.sh`. `wine/series.tsv` and `docs/wine-tree.md` are updated, with honest notes on what still needs a Mac. Merge into `main` and push (the user asked for that), and post a summary in `#mproton`.
