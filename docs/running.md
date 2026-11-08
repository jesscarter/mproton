# Running a game by hand on a mach9 bundle

To do the same with buttons (game list, Play/Stop, settings, logs, reports), use [mproton Library](../library/README.md): `library/mproton-library`.

How to start a Windows game on an Apple Silicon Mac with `launcher/mproton` and a mach9 Wine bundle, without Steam's compatibility-tool plumbing. For the Steam side, see [steam-integration.md](steam-integration.md). For how the launcher maps settings onto mach9, see [launcher/README.md](../launcher/README.md#mach9-bundle).

Status: the launcher logic is covered by the pytest suite on Linux, using mocked bundles. On 2026-11-07 it drove our signed bundle on the Mac for the first time: prefix creation, compatdb rules, `steam.exe` registry init and the Steam API all worked with `steam/tests/redirect/out/x86_64/connect_test.exe --stats` (`SteamAppId=SteamGameId=480`). The game's console output does not reach the terminal through `steam.exe`; use `PROTON_LOG=1`. The checklist at the end lists what to look at the first time.

## 1. Get a bundle

You need a mach9 `wine/` tree: `bin/wine`, `bin/wineserver`, `lib/wine/…`, `share/…`. It must be signed, because the loader in `lib/wine/aarch64-unix/wine.app` only runs with its provisioning profile ([m0-baseline.md](m0-baseline.md#signing-is-a-hard-requirement)). There are two sources:

- **athei's release** (works today). Download an `arm64-*` release from <https://github.com/athei/mach9/releases> and unpack it so that you have a directory holding `bin/wine`. It is signed by athei's team, so it runs without our own Apple membership. It has no DXVK tree, no `steam.exe` and no `lsteamclient`.
- **Our own build.** This is `work/mach9/dist/wine` after `build/mproton-build.sh` ([building.md](building.md)). It has the DXVK trees and, when built, `steam.exe` and `lsteamclient` already installed. It needs our own signing identity, or `LOADER_ADHOC=1` on a Mac with SIP and AMFI relaxed.

Leave `wine.app` untouched. Re-signing it or editing its `Info.plist` breaks it.

Optional: to give games the Steam API, install `steam.exe` and `lsteamclient` (`steam/`, see [steam/README.md](../steam/README.md)) into the bundle's default dirs:

```bash
make -C steam install WINE_SRC=/path/to/wine WINE_OBJ=/path/to/wine-build DESTDIR=$MPROTON_WINE_DIR
```

This adds files under `lib/wine/` and leaves `wine.app` alone. The launcher then starts games through `c:\windows\system32\steam.exe`. The steamclient → lsteamclient redirect works on arm64 since 2026-11-07, and Steam checks 2–4 pass ([steam/README.md](../steam/README.md)).

## 2. Install the game

Either install the game with Steam using the Windows depot (Kaon's `steam_dev.cfg` route in [steam-integration.md](steam-integration.md#mechanism-a-kaon-platform-override-convar-no-patching), or `steamcmd +@sSteamCmdForcePlatformType windows +app_update <appid>`), or just copy a Windows game directory somewhere.

## 3. Run it

Set these in a terminal:

```bash
export MPROTON_WINE_DIR=~/mach9/wine
```

```bash
export STEAM_COMPAT_DATA_PATH=~/mproton-prefixes/mygame
```

```bash
mkdir -p "$STEAM_COMPAT_DATA_PATH"
```

`MPROTON_WINE_DIR` can be the bundle itself or a directory that contains it as `wine/`. If you don't set it, the launcher looks for `wine/`, then `files/`, next to itself.

Then start the game:

```bash
python3 launcher/mproton waitforexitandrun "/path/to/Game/Game.exe"
```

The launcher fills in `STEAM_COMPAT_CLIENT_INSTALL_PATH` with the macOS Steam default if it is unset. Use a Homebrew or python.org `python3`; `/usr/bin/python3` is only a stub unless the Command Line Tools are installed.

What happens on the first run:

1. `bin/wine wineboot -i` creates `$STEAM_COMPAT_DATA_PATH/pfx/` (`WINEDEBUG=-all`), then `wineserver -w` waits for it to finish. Later runs skip this step. If `wineboot` fails, the next run tries again.
2. The launcher writes its rules to `$STEAM_COMPAT_DATA_PATH/compatdb.rules`, sets `WINE_COMPATDB_FILE` to that file, and starts the game. Without `steam.exe` in the bundle the game runs directly as `bin/wine Game.exe`, so it has no Steam API. Set `MPROTON_NO_STEAM_EXE=1` to force a direct start anyway.

Useful variants:

- `PROTON_LOG=1`: writes `~/steam-<SteamGameId>.log` (set `SteamGameId=1` when running by hand) with the rules used. Each Wine process also prints its full configuration as `compatdb:` lines on stderr ([mach9 docs/compatdb.md](https://github.com/athei/mach9/blob/main/docs/compatdb.md#logging-and-verification)).
- `python3 launcher/mproton runinprefix winecfg` (or `cmd`, `regedit`, …): runs a Wine program in the same prefix with the same rules.
- To start over, delete `$STEAM_COMPAT_DATA_PATH`.

## 4. Pick a backend

The default is no backend chosen by the launcher, so mach9's built-in rules apply: D3D10 and D3D11 run on DXMT, D3D12 on vkd3d-proton, and D3D9 on mtld3d. To change that, put one of these before the command:

| Want | Set | Rule written |
|---|---|---|
| wined3d for everything (no D3D12) | `PROTON_USE_WINED3D=1` | `dxgi=wined3d;d3d9=wined3d` |
| DXMT for D3D10/11, vkd3d-proton for D3D12 (default) | `PROTON_USE_DXMT=1` | `dxgi=dxmt` |
| mtld3d for D3D9 (default) | `PROTON_USE_MTLD3D=1` | `d3d9=mtld3d` |
| wined3d only for D3D9 | `MPROTON_D3D9=wined3d` | `d3d9=wined3d` |
| Hide D3D12 so the game picks D3D11 | `PROTON_NO_D3D12=1` | `dll_overrides=d3d12=;dll_overrides=d3d12core=` |
| DXVK | `PROTON_USE_DXVK=1` | `dxgi=dxvk;d3d9=dxvk`, but only for trees that exist |
| D3DMetal (bring your own) | `PROTON_USE_D3DMETAL=1` | `dxgi=d3dmetal`, only if `lib/wine/dxgi/d3dmetal/` exists |
| Metal HUD | `MPROTON_METAL_HUD=1` | `metal_hud=true` |
| A rule of your own | `WINE_COMPATDB='exe=Game.exe;opengl=cgl'` | passed through unchanged |

DXVK and D3DMetal need both their tree in the bundle and a `compatdb.so` that knows the value. See [graphics-dxvk.md](graphics-dxvk.md#adding-dxvk-as-a-backend-in-mach9). Until then the launcher logs that the tree is missing and keeps the built-in one. A stock `compatdb.so` also ignores `dxgi=dxvk` (it logs a warning).

Runtime knobs pass straight through to the bundle:

- `WINEMSYNC=0` turns msync off; `PROTON_NO_MSYNC=1` does the same. Note that msync is on when the variable is unset.
- `WINENCPU=<n>` limits the reported CPU count. `PROTON_CPU_TOPOLOGY=<n>:…` sets it too.
- `FEX_HARDWARE_TSO=0` makes FEX emulate x86 memory ordering instead of using hardware TSO.
- `VK_DRIVER_FILES` replaces the bundled KosmicKrisp driver.

## First-run checklist on a Mac

1. Does `wineboot -i` create the prefix without prompts? (mach9 disables Mono and Gecko by default.)
2. Is `$STEAM_COMPAT_DATA_PATH/compatdb.rules` free of `warning:` lines in the `compatdb:` stderr block, and do the `dxgi = …` / `d3d9 = …` lines match the table above?
3. Does `lsof -p <pid> | grep -i dxgi.dll` show `lib/wine/dxgi/<impl>/`?
4. Does the user directory come out as `drive_c/users/<your login>`? Proton prefixes use `steamuser`, which Steam Cloud path mapping expects. If the names differ, Cloud sync needs a fix.
5. With `steam.exe` installed in the bundle ([jesscarter/mproton#5](https://github.com/jesscarter/mproton/pull/5)), does the game start through `c:\windows\system32\steam.exe`, and does `steam.exe` find `steamclient.dylib`?

Answers from the first runs (2026-11-07): 1 yes; 4 it is `steamuser`; 5 yes, and `steamclient_init` loads the Mac `steamclient.dylib`.

## Games tested

Our bundle (signed with the paid-team profile), `launcher/mproton waitforexitandrun`, Windows depots fetched with the Steam console's `download_depot <appid> <depot>`, Mac Steam running. Apple M2, macOS 27.2.

| Date | Game (app / depot) | Engine, arch, API | Result |
|---|---|---|---|
| 2026-11-07 | Queen's Domain Demo (4081520 / 4081521) | UE4, x86_64, D3D11 | Renders, sound works. Steam API connected after the steamclient stand-ins (pipe, user, persona, `BIsSubscribed`, stats received, callback loop) |
| 2026-11-07 | ULTRAKILL Demo (1230260 / 1230261) | Unity 2019.4 (Mono), x86_64, D3D11 FL 11.1 | Plays fine. No Steam API in the demo. Player.log: Discord SDK init fails (Discord not running), 50 `NullReferenceException`s from game scripts, "Uploading Crash Report" is Unity's exception reporter, not a crash. Unity prints the GPU vendor as garbage (DXMT adapter vendor string) |
| 2026-11-07 | Emily is Away (417860 / 417861) | Unity 2017, **i386** (WoW64), D3D11 | Looks right. First 32-bit game with Steam: i386 stand-in -> WoW64 lsteamclient -> Mac client (SteamClient020, user, stats, utils, callback loop). No achievement calls happened in the session |
| 2026-11-07 | Lunacid (1745510, Windows build installed by Steam) | Unity 2020.3 (Mono), x86_64, D3D11 | Works (run from mproton Library). Ships `steam_api64.dll`; Steam connection not checked (no `+steamclient` trace) |
| 2026-11-07 | Castlevania: Belmont's Curse (4231820, Windows build installed by Steam) | Unity 2022.3 (IL2CPP), x86_64, D3D11 FL 11.1 | Works (run from mproton Library). **Bug:** the resolution setting in the video options shows no resolution; display-mode enumeration (winemac.drv / DXMT `GetDisplayModeList`) not yet investigated. Steam connection not checked |
