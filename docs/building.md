# Building mproton's Wine bundle

`build/mproton-build.sh` takes a fresh checkout of this repository to a runnable Wine bundle on an Apple Silicon Mac. It drives athei's [mach9](https://github.com/athei/mach9) build scripts, which build ARM64 Wine 11.20, FEX, KosmicKrisp, DXMT and vkd3d-proton, and adds mproton's own parts:

- the Proton Wine patch series (`wine/apply.sh`), when it is in the checkout;
- DXVK as an extra Direct3D backend (`graphics/dxvk/`);
- `lsteamclient` and `steam.exe` (`steam/`).

The result is `work/mach9/dist/wine`. The launcher runs games on it (`MPROTON_WINE_DIR`, see [running.md](running.md)).

## Quick start

On the Mac, once the [prerequisites](#prerequisites) are installed:

```bash
LOADER_ADHOC=1 build/mproton-build.sh
```

Or, with a real provisioning profile:

```bash
LOADER_PROFILE=~/signing/mproton-loader.provisionprofile build/mproton-build.sh
```

A run from scratch takes roughly 25 minutes for the Wine stack on an M2 (M0 measured 22), plus DXVK, Steam and the bundle. Each later run only redoes what changed. Check the state at any time with:

```bash
build/mproton-build.sh --status
```

## Prerequisites

| What | How | Needed by |
|---|---|---|
| Apple Silicon Mac, macOS 26.4 or newer | mach9's floor is 26.4 (`MACOS_FLOOR` in its `bin/lib/profile.sh`). M0 runs 27.2 | everything |
| Full Xcode | Install it from the App Store or developer.apple.com. The Command Line Tools alone are not enough. The script sets `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` itself, so `xcode-select` can keep pointing at the CLT | DXMT (Metal shaders), `codesign` |
| Metal toolchain | `xcodebuild -downloadComponent MetalToolchain` (about 840 MB). Check it with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun -f metal` | DXMT |
| Homebrew formulae | See the command below the table | mach9: Wine's configure, Mesa's host tools, the build tools; DXVK: `meson`, `ninja`, `glslang` |
| Rust | `curl https://sh.rustup.rs -sSf \| sh`. The stable toolchain targets `aarch64-apple-darwin` already | `compatdb.so`, which the bundle step builds |
| llvm-mingw | `scripts/setup.sh` downloads release 20261104 to `toolchains/llvm-mingw`. It uses the GitHub CLI: `brew install gh && gh auth login`. The build runs it itself when the directory is missing | every PE build |
| Python 3 | Homebrew's or python.org's. `/usr/bin/python3` is only a stub without the CLT | Mesa's build scripts, DXVK, the launcher |
| git | Comes with the Xcode tools | workspace |
| Signing | `LOADER_ADHOC=1` or `LOADER_PROFILE=...`, see [Signing](#signing) | bundle |

The Homebrew formulae are the ones in mach9's `pins/brew-formulae`, at mach9 `d176766`:

```bash
brew install freetype gnutls sdl2-compat sdl3 bison pkgconf llvm spirv-llvm-translator spirv-tools cmake ninja meson glslang ccache
```

The wine step checks the list against `work/mach9/scripts/pins/brew-formulae` before it builds, and prints the `brew install` line for anything missing. After a mach9 bump, that file is the reference.

Plan for a lot of disk space. The Mesa, LLVM 15 (for DXMT), Wine, x86 link library, GStreamer and FEX trees together take several tens of GB.

### Signing

The Wine loader needs the restricted entitlement `com.apple.developer.cross-architecture-support` ([m0-baseline.md](m0-baseline.md#signing-is-a-hard-requirement)). The bundle step refuses to start until you choose one of these:

- **`LOADER_PROFILE=<file>`**: a Mac App Development or Developer ID provisioning profile from a **paid** team, with the Cross-architecture Compatibility Framework capability, and that team's certificate in your keychain. mach9 reads the team, bundle ID and identity from the profile. Don't rely on the default `scripts/signing/loader.provisionprofile`: it is athei's, and it won't sign for us.
- **`LOADER_ADHOC=1`**: the loader is signed ad hoc and claims the entitlement without a profile (`signing/mach9-adhoc-loader.patch`). macOS only honours that with SIP disabled **and** the `amfi_get_out_of_my_way=1` boot-arg (with Permissive Security). SIP-off alone gets the process killed: exit 137, "Code Signature Invalid". Run `tools/amfi-check.sh` after rebooting with the boot-arg to see whether macOS 27 accepts it. This setup is for a local build only. Never hand such a bundle to anyone.

`bundle-wine.sh` runs the bundle's smoke tests (it starts Wine), so the bundle step itself fails when the signature isn't honoured.

## Steps

```
build/mproton-build.sh [--from <step>] [--force] [--clean] [--bundle-arg <arg>]... [step...]
```

With no steps given, it runs all of them in order: `workspace`, `wine`, `dxvk`, `steam`, `bundle`. Name steps to run only those (`build/mproton-build.sh dxvk bundle`), or use `--from steam` to run `steam` and everything after it.

| Option | Effect |
|---|---|
| `--force` | Runs the steps even when their stamps say they are current. mach9's own scripts still skip what they consider current (see below) |
| `--clean` | `--force`, plus wiping the build trees: `build-wine.sh --clean` (all of mach9's trees and dists), DXVK's work dir, and the steam objects |
| `--bundle-arg <arg>` | Passes `<arg>` to `bundle-wine.sh`. Repeatable. For example `--runtime-only` (no SDK or tests in the bundle), `--keep-symbols`, `--symbols`, `--strict-floor` |
| `--status` | Prints each step's state and exits |

| Variable | Default | Meaning |
|---|---|---|
| `LOADER_ADHOC`, `LOADER_PROFILE` | none | Signing, see above |
| `MPROTON_WORK` | `work/mach9` | The workspace (mach9's root) |
| `MINGW_DIR` | `toolchains/llvm-mingw` | llvm-mingw |
| `DEVELOPER_DIR` | `/Applications/Xcode.app/Contents/Developer` | Xcode |
| `MPROTON_BUNDLE_DEST` | `<work>/dist` | Where the bundle goes, as `<dir>/wine` |
| `MPROTON_SKIP_DXVK=1`, `MPROTON_SKIP_STEAM=1` | unset | Leave a component out |

### 1. workspace

Sets up `work/mach9/` in mach9's layout:

- **`scripts/`**: mach9 at `MACH9_COMMIT` (`build/pins.env`). The patches in [`build/mach9-patches/series`](../build/mach9-patches/series) are applied with `git apply`:
  1. `signing/mach9-adhoc-loader.patch`: the `LOADER_ADHOC=1` mode.
  2. `0002-fex-stage-data-under-destdir.patch`: `build-fex.sh` stages FEX through a `DESTDIR`. FEX installs its AppConfig JSON to `DATA_DIRECTORY`, an absolute path fixed at configure time (`/usr/local/share/fex-emu`), which `cmake --install --prefix` doesn't move. With `DESTDIR` it lands in the scratch install dir with everything else, and nothing compiled changes. This replaces M0's hand reconfigure with `-DDATA_DIRECTORY=...`. The patch is upstreamable as is.
  3. `0003-dxvk-backend-trees.patch`: DXVK as a third Direct3D tree, see [bundle](#5-bundle).

  Then the four `pins/*-src.ref` files, which name branches upstream (`macos-patched`), are overwritten with the commits in `build/pins.env`, so FEX, Mesa, DXMT and vkd3d-proton stay fixed too.
- **`src/`**: athei/wine. A blobless clone (`--filter=blob:none`), so the first clone downloads history without old file contents.
  - If `wine/apply.sh` exists in this checkout, the base is the commit in `wine/base.ref` (else `WINE_COMMIT`), and `wine/apply.sh <src>` applies the Proton series on top as commits on branch `mproton`, with a fixed committer so the hashes are the same on every machine.
  - Without `wine/apply.sh`, `src/` is plain athei at `WINE_COMMIT`. The run says so in its summary. That Wine lacks the steamclient → lsteamclient redirect.
- **llvm-mingw**: `scripts/setup.sh` runs when `toolchains/llvm-mingw` is missing.

Local changes are never thrown away silently. If `scripts/` or `src/` has uncommitted changes when the step needs to move it (for example, `scripts/` with the ad hoc patch applied by hand, as in M0), they are saved to `work/mach9/.mproton/backups/<dir>-<time>-<pid>-<commit>.diff` first. Untracked files, such as your own profile in `scripts/signing/`, are not touched.

This is the only step that also runs on Linux. That is useful to check that the pins resolve and the patches apply.

### 2. wine

Runs mach9's `bin/build-wine.sh` with `MINGW_DIR` and `DEVELOPER_DIR` set. That script builds, in order:

1. Mesa (KosmicKrisp) and the Vulkan loader
2. GStreamer
3. Wine (`--enable-archs=arm64ec,aarch64,i386`)
4. the x86 link-library tree and the Wine SDK (`build/sdk`)
5. FEX
6. DXMT
7. vkd3d-proton

Before that, the step checks for full Xcode, the Metal toolchain and the Homebrew formulae.

mach9's component scripts skip a component whose staged dist already matches its source commit and script hash (`skip_current_dist`), and Wine's own `make` is incremental. So a re-run after an interruption continues where it stopped. The first run after this tooling lands rebuilds FEX once, because patch 0002 changes `build-fex.sh`, which is part of FEX's dist key, and resets `DATA_DIRECTORY`.

### 3. dxvk

Runs `graphics/dxvk/build.sh arm64x i386` (see [graphics-dxvk.md](graphics-dxvk.md)) with its work and output under `work/mach9/build/mproton-dxvk/`. The step:

- records where the build staged its output (`out/dxvk-<describe>/`);
- checks that the staged `COMMIT` is the one in `dxvk.pin`;
- leaves the DLLs as plain (native) DLLs. The bundle step marks its own copies as Wine builtins.

If `graphics/dxvk/build.sh` isn't in the checkout, the step is skipped with a message, and the bundle has no DXVK tree. A shim maps `sha256sum` to `shasum -a 256` where macOS lacks it.

### 4. steam

Builds `lsteamclient.dll` (ARM64X and i386), `lsteamclient.so` (arm64 Mach-O) and `steam.exe` (aarch64 and i386) with `steam/Makefile`. It builds against the mach9 Wine source and build tree (`WINE_SRC=work/mach9/src`, `WINE_OBJ=work/mach9/build/wine`), with mach9's macOS floor as the deployment target. Objects go to `work/mach9/build/mproton-steam/`. If `steam/Makefile` isn't in the checkout, the step is skipped with a message.

### 5. bundle

1. Runs mach9's `bin/bundle-wine.sh --yes` (plus any `--bundle-arg`), with `DXVK_DIST` set when the dxvk step staged DXVK. With patch 0003, mach9 then treats DXVK as one more Direct3D implementation:
   - `lib/wine/dxgi/dxvk/{aarch64,i386}-windows/`: DXVK's `dxgi`, `d3d10core` and `d3d11`, Wine's `d3d10` and `d3d10_1`, and vkd3d-proton's `d3d12` and `d3d12core`. This is Proton's pairing: DXVK's DXGI presents vkd3d-proton's swapchains.
   - `lib/wine/d3d9/dxvk/{aarch64,i386}-windows/d3d9.dll`. DXVK's `d3d8` is not installed, so D3D8 stays on wined3d as in the other trees.
   - The copies are marked as Wine builtins with the build tree's winebuild (64-bit) and the SDK's (i386).
   - Because `dxvk` is appended to `DXGI_IMPLS` and `D3D9_IMPLS`, mach9's own machinery covers it. `check_trees_arm64x` requires every 64-bit DLL to be ARM64X. The default-dir markers are still stamped from the trees ahead of DXVK, since DXVK's DLL names are a subset of theirs. The per-tree smoke test (`smoke: <arch> on dxgi/dxvk`) loads every DLL in the tree under a `WINE_COMPATDB` rule. Step 5 checks the files.
   - compatdb gains `dxvk` as a value of `dxgi` and `d3d9` (`compatdb/core/src/value.rs`). A rule `dxgi=dxvk;d3d9=dxvk` selects the trees. The launcher writes that for `PROTON_USE_DXVK=1`.
2. Installs lsteamclient and steam.exe with `make -C steam install DESTDIR=<bundle>`. They go into `lib/wine/{aarch64,i386}-windows/` and `lib/wine/aarch64-unix/`, next to Wine's builtins, which is where the launcher looks for `steam.exe` ([running.md](running.md)). The step then checks that:
   - every file landed;
   - the PE files carry Wine's builtin marker;
   - `lsteamclient.so` links nothing outside the bundle (`otool -L`).

   `wine.app` and its signature are untouched. The loader's `disable-library-validation` entitlement lets the ad-hoc-linked `lsteamclient.so` load, as it does for Wine's other unix modules.
3. Writes `share/mproton/BUILD-INFO`: the pins, components and signing mode of the build.

`bundle-wine.sh` deletes and rebuilds the bundle from scratch on every run, so this step reruns completely whenever any of its inputs changes. That includes the signing mode, the profile, the DXVK or steam build, and the Wine build.

## Fixing the Wine series: batch fixes, build once

A full run takes 25+ minutes, mostly the bundle step, the 32-bit Wine tree, FEX and the SDK. A fix to one Wine file shouldn't pay for all of that. The loop is:

1. **Edit the series in a scratch worktree, never in `work/mach9/src`:**
   ```bash
   wine/scripts/series-edit.sh start            # work/series-edit, before the regen patch
   # fix, commit (one commit per fix), repeat
   wine/scripts/series-edit.sh finish           # regen patch back on top, wine/patches re-exported and checked
   ```
2. **Compile-check:** `build/mproton-build.sh --check` moves `work/mach9/src` to the new series (only changed files are rewritten) and runs `make -k` in the existing Wine build tree. It compiles only what changed and prints **every** error, not just the first. Fix them all (back to step 1) and check again. This takes seconds to a few minutes.
3. **Once it compiles:** run the full build (`build/mproton-build.sh --from workspace`) once, for the smoke tests and the bundle.

Several fixes, or several cloud branches merged together, go through one `--check` loop and one full build.

What keeps rebuilds incremental, and what used to make them full:

- **Timestamps, not contents.** make rebuilds what is *newer* than its output. If `configure` is newer than `config.status`, make reruns configure. That regenerates `config.h`, which mach9 then edits in place (sonames), so it always counts as changed, and every file includes it. Result: a full rebuild.
- **Moving `src`:** the workspace step applies the series in a scratch worktree and checks `src` out straight at the result, so git rewrites only files whose contents differ (`build/mproton-build.sh`, `apply_series_incremental`). Checking out the base and re-applying, or editing `src` by going to the commit before the regen patch and back, rewrites `configure` with the same content and a new timestamp.
- **Backstop:** before each Wine build, `configure` and `include/config.h.in` get the timestamp of their make target back when their content is the same as at the last Wine build (`restore_generated_mtimes`).
- **ccache** (on by default when installed, `MPROTON_CCACHE=0` turns it off): compiler output is cached by content, so a rebuild that make starts anyway mostly comes from the cache. The cache only helps once a build has filled it; `ccache -s` shows the hit rate.
- **`make -k`** (mach9 patch 0004): a failing build lists every error.

## Layout

| Path | What it is |
|---|---|
| `build/mproton-build.sh` | The orchestrator |
| `build/lib/common.sh` | Its helpers (stamps, git, output) |
| `build/pins.env` | mach9, athei/wine, FEX, Mesa, DXMT and vkd3d-proton commits |
| `build/mach9-patches/` | The patch series for mach9 |
| `work/mach9/scripts/` | mach9, patched |
| `work/mach9/src/` | Wine source |
| `work/mach9/src-fex/`, `src-mesa/`, `cache/` | mach9's clones and downloads |
| `work/mach9/build/<component>/` | mach9's build trees: `wine`, `x86libs`, `sdk`, `mesa`, `gstreamer`, `fex`, `dxmt`, `vkd3d-proton`, `compatdb` |
| `work/mach9/build/mproton-dxvk/`, `mproton-steam/` | DXVK's and steam's trees |
| `work/mach9/dist/wine/` | The bundle |
| `work/mach9/.mproton/stamps/` | One file per finished step, holding the inputs it ran with |
| `work/mach9/.mproton/logs/` | The full output of every run |
| `work/mach9/.mproton/backups/` | Local changes saved before a checkout moved |

## Bumping a pin

- **mach9:**
  1. Set `MACH9_COMMIT`.
  2. Run `build/mproton-build.sh workspace`. On Linux that is enough to see whether every patch still applies; the step stops at the first one that doesn't.
  3. Rebase those patches. Regenerate each from a mach9 checkout with `git diff`.
  4. Compare `pins/brew-formulae` and `MACOS_FLOOR` with the docs above.
- **athei/wine:** set `WINE_COMMIT`. Once the Proton series is in, `wine/base.ref` decides the base instead, and the series has to be rebased with it.
- **FEX, Mesa, DXMT, vkd3d-proton:** set the commit (`git ls-remote https://github.com/athei/<repo> refs/heads/macos-patched`). mach9 rebuilds that component and everything the bundle takes from it.
- **DXVK:** `graphics/dxvk/dxvk.pin`.

## Troubleshooting

- **`another mproton-build.sh holds .../.mproton/lock`**: a run is in progress, or one was killed with `kill -9` or a power loss. If none is running, `rmdir work/mach9/.mproton/lock`.
- **`<patch> does not apply to mach9 <commit>`**: `MACH9_COMMIT` and the patch disagree. Rebase the patch, or set the pin back to the commit the series was made for (the comment in `build/pins.env`).
- **`Permission denied` under `/usr/local/share/fex-emu`** while FEX installs: `scripts/` doesn't carry patch 0002. Run `build/mproton-build.sh workspace` and check `git -C work/mach9/scripts diff --stat bin/build-fex.sh`.
- **`Xcode ... has no Metal toolchain`**, or DXMT fails to find `metal`/`metallib`: run `xcodebuild -downloadComponent MetalToolchain`. If Xcode isn't in `/Applications`, set `DEVELOPER_DIR`.
- **`Homebrew formulae missing`** or mach9's `Error: not installed in /opt/homebrew`: run the `brew install` line it prints. mach9 expects Homebrew in `/opt/homebrew`.
- **`configure found <lib> for SONAME_..., which the allowlist does not account for`**: a Homebrew library that mach9's configure allowlist doesn't expect got picked up. Uninstall it, or follow the message.
- **`... was configured without GStreamer`**, or any change to configure flags or the deployment target: run `build/mproton-build.sh --clean wine`. This rebuilds everything mach9 builds.
- **`<dir> is not empty and has no .wine-build-tree`** (mach9's `claim_tree`): a tree mach9 wants to fill holds something it didn't make. Move that directory away. Don't point mach9's `*_DIR` overrides at existing directories.
- **The bundle step says `no signing set up for the loader`**: set `LOADER_PROFILE` or `LOADER_ADHOC=1` ([Signing](#signing)).
- **`Error: no provisioning profile at ...`, `the provisioning profile does not grant ...` or `no valid code signing identity of team ...`** from `bundle-wine.sh`: the profile is missing or lacks the capability, or its team's certificate isn't in the keychain (`security find-identity -v -p codesigning`).
- **The smoke test is killed: `exit 137` / `SIGKILL (Code Signature Invalid)`** in `~/Library/Logs/DiagnosticReports`: an ad-hoc-signed loader without the boot-arg. Check `nvram boot-args` and run `tools/amfi-check.sh`. Its verdict says whether ad hoc signing can work on this Mac.
- **Smoke test failures on `dxgi/dxvk`**, or `compatdb: warning: ... dxgi=dxvk`: the `compatdb.so` in the bundle doesn't know `dxvk`, because patch 0003 isn't in `scripts/`. Re-run `workspace`, then `bundle`.
- **`DXVK is in the checkout but not built`** at the bundle step: run `build/mproton-build.sh dxvk`, or `MPROTON_SKIP_DXVK=1` for a bundle without it.
- **`glslang not found`** or the `meson` errors from `graphics/dxvk/build.sh`: `brew install glslang meson ninja`.
- **`scripts/setup.sh` fails in `gh release download`**: `gh auth login` first. Or download `llvm-mingw-20261104-ucrt-macos-universal.tar.xz` from mstorsjo/llvm-mingw's releases, unpack it in `toolchains/`, and symlink it as `toolchains/llvm-mingw`.
- **`lsteamclient.so links outside the bundle`**: the unix side picked up a Homebrew or build-tree library. The `otool -L` output above the error names it.
- **Warnings about Mach-O files above the floor** (`minos`): Homebrew's bottles are built for the host's macOS. That's harmless for a local bundle. `--bundle-arg --strict-floor` turns it into an error, for a release build on a floor-version host.
- **Games don't see Steam even with lsteamclient installed**: a known gap. The steamclient → lsteamclient redirect needs the Proton Wine series, and even then its fault hook is missing on arm64 (see [steam/README.md](../steam/README.md#wine-side-requirements)).
- **A step reruns although nothing changed**: `build/mproton-build.sh --status` shows which step's inputs changed. Editing anything under `steam/`, `graphics/dxvk/`, `wine/` or `build/` counts as a change.
- **`No space left on device`**: delete `work/mach9/build/mproton-dxvk/work` (rebuilt in a few minutes), old logs and backups under `.mproton/`, and `dist/wine-symbols` if you used `--symbols`. Don't delete mach9's trees (`build/dxmt/llvm15` in particular takes long to rebuild) unless you mean to rebuild them.
- **Starting over**: delete `work/mach9/.mproton/stamps`. Every step then checks its state again; mach9's builds stay incremental. Delete `work/mach9` only to really start from nothing.
