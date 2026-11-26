# mproton Library

A small Mac app that turns testing a Windows Steam game into button clicks: it lists the Windows games already on this Mac, and lets you start (**Play**) and stop (**Stop**) each one, change its settings, read its logs and save a bug report. It runs games with `launcher/mproton waitforexitandrun`, the same command as in [../docs/running.md](../docs/running.md), and changes nothing in the launcher or in the Steam client.

## Start it

From a [release download](https://github.com/jesscarter/mproton/releases): open **mproton Library**, kept in the same folder as the download's `wine` folder. The app carries its own copy of `launcher/` and `library/`.

From a checkout:

```bash
library/mproton-library
```

This starts a small server on `http://127.0.0.1:8417/` and opens that page in your browser. If the library is already running, the command just opens the page again. Options: `--port N` and `--no-browser`.

To use it as a Mac app instead (Finder, Spotlight, the Dock), build it once:

```bash
library/make-app.sh
```

This creates `~/Applications/mproton Library.app`, which runs `library/mproton-library` from this checkout. It shows the library in its own window, with a Dock icon and menus. It starts the server when it opens and stops it when you quit (⌘Q, closing the window, or the page's **Quit** button). If a library server is already running on port 8417, for example one started from a terminal, the app uses it and leaves it running when it quits. Games that are still running keep running either way. The server's output goes to `~/Library/Logs/mproton-library.log` (**View → Show Server Log**). Run `make-app.sh` again after moving the checkout.

The app is `library/app/main.swift`, compiled with `swiftc` from Xcode or the Command Line Tools. Its icon is drawn at build time by `library/app/make-icon.swift`. It's signed ad hoc, so it runs on the Mac that built it. Releases build it with `make-app.sh --bundle`, which copies the tracked files of `launcher/` and `library/` into `Contents/Resources/mproton` with a `VERSION` file; `build/mproton-release.sh` then signs it with the Developer ID, notarizes and staples it, and packs it beside `wine/`. The app sets `PYTHONDONTWRITEBYTECODE=1`, so Python never writes `__pycache__` into the signed bundle. Without `swiftc`, `make-app.sh` builds the older script bundle instead: no window or Dock icon, and it opens the page in your browser.

## What you need

- A signed mproton Wine bundle: either a [release download](https://github.com/jesscarter/mproton/releases) or your own build (`build/mproton-build.sh`, see [../docs/building.md](../docs/building.md)). The library uses the first of: the **Paths** setting, `MPROTON_WINE_DIR`, the `wine` folder next to mproton Library.app, a build in this checkout, a build in `~/mproton` (so it also works from a git worktree, which has no `work/`), and `~/mproton-wine`. The launcher carries its own `filelock.py`; a Proton reference tree in `ref/proton` is optional and only used if present.
- Steam for Mac, running and signed in, if the game uses the Steam API. The library never asks for or enters Steam credentials.
- Nothing else. The library uses only the Python standard library. `mproton-library` uses Homebrew's `python3` if it is installed, otherwise `/usr/bin/python3`, which works once Apple's Command Line Tools (or Xcode) are installed; without them the app shows a page that offers to install them. The launcher runs under the same Python. The library never runs `git` unless it is in a checkout: without the Command Line Tools, `/usr/bin/git` would only offer to install them.

## What's on the page

**Top bar**

- **Steam running / not running**: whether a `steam_osx` process exists.
- **Bundle signed (TEAMID)**: whether the bundle was found and whether `codesign --verify --strict` passes on its `wine.app` loader. An ad hoc signature shows as a warning. Hover over it to see the bundle path and `share/mproton/BUILD-INFO`.
- **Get a Windows game**: finds the `download_depot` commands for a game's Windows build. See [Get a Windows game](#get-a-windows-game-download_depot) below.
- **Steam console** opens Steam's console (`open steam://open/console`), where `download_depot` lines go.
- **Windows downloads** switch: see [Windows downloads](#windows-downloads-steam_devcfg) below.
- **Refresh**: scans for games again and re-fetches the Steam launch configs. **Reports folder** opens `~/mproton-reports`. **Paths** sets the launcher, `ref/proton`, the bundle and the Python to use.

**Game list**

The list shows Windows games from two places:

1. `download_depot` output, in `Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_<appid>/depot_<depot>/`.
2. Steam library folders, read from `steamapps/libraryfolders.vdf` and `appmanifest_*.acf`. An installed game is listed only if it is a Windows build. That means the Windows launch target from Steam's launch config exists in its install folder. Mac builds are not listed. If the launch config can't be fetched (for example, when you're offline), any install that contains a `.exe` is listed and marked "Windows build not verified".

The name comes from Steam's app info, or from the appmanifest. The picture comes from Steam's local `appcache/librarycache/<appid>/` folder. Nothing is downloaded for it.

**Launch exe**

The library reads the game's launch config from `https://api.steamcmd.net/v1/info/<appid>` and uses the first Windows entry whose file exists. It prefers the default launch type, 64-bit, and no beta branch. It finds the file even if the capitalisation differs from Steam's spelling, and passes Steam's launch arguments too.

The answers are cached in `~/Library/Application Support/mproton-library/cache/steamcmd/`. To fetch one again, use **Re-check launch config**.

If there is no launch config, the library guesses: it picks the shallowest `.exe` that isn't a helper such as a crash handler or redistributable installer. The **Launch exe override** setting always wins over both.

**Play / Stop**

**Play** runs:

```bash
cd <exe folder or Steam's workingdir>
MPROTON_WINE_DIR=<bundle> \
STEAM_COMPAT_DATA_PATH=~/mproton-prefixes/<appid> SteamAppId=<appid> SteamGameId=<appid> PROTON_LOG=1 \
  <python3> launcher/mproton waitforexitandrun <exe> [Steam's args] [your extra args]
```

The settings below add their own variables. Under **Last command** the page shows the exact command and variables it used.

- The prefix is `~/mproton-prefixes/<appid>`. It is created on the first Play, which takes longer. Prefixes from earlier manual runs (such as `~/mproton-prefixes/emily`) are not reused.
- The page shows **Running** with the launcher's process ID, and then **Exited with code N**.
- **Stop** runs `WINEPREFIX=~/mproton-prefixes/<appid>/pfx <bundle>/bin/wineserver -k`. That ends every Wine process in that game's prefix, and nothing else. If the launcher still hasn't exited 10 seconds later, it gets `SIGTERM`.

Run state is kept only in memory. If you quit and restart the library while a game is running, the game keeps running, but the page shows it as not started. **Stop** still works, because it acts on the prefix.

**Settings (per game)**

| Setting | What it sets |
|---|---|
| Graphics backend | Default: no variable, so mach9's built-in rules apply (DXMT for D3D10/11, vkd3d-proton for D3D12, mtld3d for D3D9). DXMT: `PROTON_USE_DXMT=1`. DXVK: `PROTON_USE_DXVK=1`. wined3d: `PROTON_USE_WINED3D=1`. See [../launcher/README.md](../launcher/README.md#backends-as-compatdb-rules). |
| WINEDEBUG | `WINEDEBUG=<value>`. Leave it empty to use the launcher's `PROTON_LOG` default (`+timestamp,+pid,+tid,+seh,+unwind,+threadname,+debugstr,+loaddll,+mscoree`), which is very verbose. `+steamclient,err` gives a short log that still shows Steam API setup. |
| Extra environment | One `KEY=VALUE` per line, for example `PROTON_NO_D3D12=1`, `MPROTON_METAL_HUD=1` or `WINEMSYNC=0`. |
| Extra arguments | Added after Steam's launch arguments. |
| Launch exe override | An absolute path, or a path relative to the game folder. |

The settings are stored in `~/Library/Application Support/mproton-library/settings.json`. Backend and debug variables inherited from the shell that started the library (`PROTON_USE_*`, `WINEDEBUG`, `SteamAppId`, and so on) are removed first, so only the page's settings apply.

**Logs**

For each game the page lists these logs. **View** shows the end of a log on the page and keeps it updated while the game runs. **Open** opens it in Console, or in TextEdit if it isn't a `.log` file.

- `~/steam-<appid>.log`: the launcher's `PROTON_LOG` output. The launcher deletes and rewrites it on every start.
- The launcher's own console output, saved in `~/Library/Application Support/mproton-library/runs/<appid>.out`, with the command line at the top.
- Unity's log: `Player.log` or `Player-prev.log` under `<prefix>/pfx/drive_c/users/*/AppData/LocalLow/<company>/<product>/`, or `output_log.txt` for Unity versions before 2018.3.
- Unreal's logs: `AppData/Local/<Project>/Saved/Logs/*.log`.
- `*.log` files in the game folder.

**Save report** writes `~/mproton-reports/<appid>-<name>-<time>.zip`. The zip contains:

- `report.txt`: the game, exe, settings, last run (command, environment, exit code), bundle path, `wine --version`, the bundle's signature, the repo HEAD, the Steam and Windows-downloads state, and the macOS version and Mac model.
- `settings.json`
- every log listed above
- the prefix's `compatdb.rules`
- the bundle's `BUILD-INFO`

## Get a Windows game (`download_depot`)

This is the per-game way to get a Windows build. It changes nothing in Steam, so games you have as Mac builds stay Mac builds. Use it instead of the global [Windows downloads](#windows-downloads-steam_devcfg) switch when you want only one game.

1. Type a game's name, or its appid, and press **Search**. Names go to Steam's store search (`store.steampowered.com/api/storesearch`). An appid is looked up directly, which also finds apps the store search doesn't list.
2. Pick a result. The library fetches its depots from `api.steamcmd.net` and shows one `download_depot <appid> <depot>` line per depot a Windows install needs, with **Copy** buttons. **Copy** uses the browser's clipboard, or `pbcopy` through the server if the browser denies it; the server copies only `download_depot` lines.
3. Press **Open Steam console**, paste each line and press Return. Steam prints "Depot download complete" when it's done, with no progress bar. Then press **Refresh**.

Which depots are listed as needed: those for Windows or for all platforms (no `oslist`), with a public build, for English or no language. These are shown but not needed, with the reason: Steamworks shared redistributables (`depotfromapp`, for example the Visual C++ installers), DLC (`dlcappid`), other languages, low-violence versions, depots with only beta builds, and 32-bit depots when a 64-bit one exists. A depot already in `steamapps/content` shows as "downloaded".

Limits:

- `download_depot` only works for games you own (demos are free to download).
- Each depot goes into its own `depot_<id>` folder. The library looks for the exe in all of them, but the game runs from the folder its exe is in. If a game's files are split across depots and it won't start, copy them into one folder and set that as the **Launch exe override**.
- Steam doesn't update these files. Run the same line again for a newer build.

## Windows downloads (`steam_dev.cfg`)

This switch is Mechanism A from [../docs/steam-integration.md](../docs/steam-integration.md). It adds or removes the line `@sSteamCmdForcePlatformType windows` in `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_dev.cfg`. It is **off by default**, and the page asks before changing it. When it's on:

- **It is global.** Steam acts as a Windows client for every game: it offers Install for Windows games and downloads Windows depots. Games that have both builds may be switched to their Windows build when they update.
- **Steam stops updating itself** while `steam_dev.cfg` exists.
- **Steam only reads the file when it starts.** So after any change, quit Steam (Steam → Quit Steam) and open it again. The page shows a "Restart Steam" banner while the running Steam started with the other setting.

Only that one line is changed:

- If the file doesn't exist, it is created with just that line. Turning the switch off then deletes the file, so that Steam updates itself again.
- If the file already exists, every other line is left exactly as it was. If the file already set `@sSteamCmdForcePlatformType` to another value, that line is replaced in place, and put back when you turn the switch off.
- Before every change, a copy of the old file (or a note that there was no file) goes to `~/Library/Application Support/mproton-library/steam_dev.cfg.backups/`.

Not yet tested: what Steam does with a game installed as a Windows build after the switch is turned off. It may keep that build until the game updates or you verify its files, and then replace it with the Mac build. Games fetched with `download_depot` are not affected either way, because Steam does not manage `steamapps/content`.

The library never patches, injects into or otherwise changes the Steam client (Mechanism B is not used).

## Design choice: a local web page served by Python, in a thin native window

- **Why this way.** The library itself needs no build step: it uses only the Python standard library, which this Mac already has for the launcher. Every action is a plain HTTP call, so it can be tested with `curl`, and the parsing and `steam_dev.cfg` logic has unit tests.
- **The window.** `mproton Library.app` is a small Swift program (about 250 lines) that shows the page in a WKWebView, the web view Safari uses, and starts and stops the server. All the UI stays in `static/index.html`. Electron would add about 150 MB of Chromium for one page. Tauri uses the same WKWebView but adds a Rust and npm toolchain. A SwiftUI rewrite would duplicate the page.
- **The cost.** It still needs Python 3. On a Mac without the Command Line Tools, `/usr/bin/python3` only offers to install them. Without the app, the page runs in a browser tab, and closing the tab doesn't stop the server; use **Quit** for that.
- **Safety.** The server listens only on `127.0.0.1`. It only answers requests addressed to `127.0.0.1` or `localhost`, which blocks DNS-rebinding attacks. Every action needs an `X-Mproton: 1` header, which a web page on another site can't send without a CORS preflight, and the server never grants one. Files can only be opened if they are on the library's own lists.

## Files

| Path | What it is |
|---|---|
| `mproton-library` | Start script: picks a Python, then runs `mproton_library.py` |
| `mproton_library.py` | Server, game discovery, launch-config lookup, Play/Stop, logs, reports, `steam_dev.cfg` switch |
| `static/index.html` | The page (plain HTML, CSS and JavaScript; no external libraries) |
| `make-app.sh` | Builds `mproton Library.app` (native with `swiftc`, else a script bundle) |
| `app/main.swift` | The app: window, menus, starting and stopping the server |
| `app/make-icon.swift` | Draws the app icon at build time |
| `tests/test_library.py` | Unit tests (including depot filtering on Hypernet Explorer's real app info): `python3 -m unittest discover -s library/tests` |

Environment overrides, mainly for tests: `MPROTON_STEAM_ROOT`, `MPROTON_LIBRARY_HOME` (state folder), `MPROTON_PREFIXES`, `MPROTON_REPORTS`, `MPROTON_LIBRARY_PORT`, `MPROTON_PYTHON`.

## Verified on this Mac (2026-11-07)

Tested on an Apple M2 with macOS 27.2, Steam running, and the bundle `mproton beff3ce` (signed, team N4MNUWFBFD):

- The list showed the three `download_depot` games: Emily is Away (417860), Queen's Domain Demo (4081520) and ULTRAKILL Demo (1230260). Each had its exe from Steam's launch config. The 23 Mac installs in the Steam library were correctly left out.
- **ULTRAKILL Demo**, started with Play and `WINEDEBUG=+steamclient,err`:
  - `~/steam-1230260.log` shows `Loaded host steamclient from ".../steamclient.dylib"`.
  - `Player.log` shows `Initialize engine version: 2019.4.0f1` and `Direct3D 11.0 [level 11.1]`, `Renderer: Apple M2`.
  - A 1470×956 "ULTRAKILL" window was on screen.
  - Stop: `wineserver -k` returned 0, and the run ended with exit code 0, marked "stopped by you".
  - Save report wrote a 7-file zip.
- **Emily is Away**, started with Play: Unity's own "Emily Is Away Configuration" (resolution) window came up and waited for a click. This game's Unity version shows that window before the game itself. Stop ended it.
- **Windows downloads switch**: tested with the file absent, with a file holding other lines, and with a file that set the same ConVar to `macos`. In each case, turning it on then off left the file byte-for-byte as before, or absent again. `steam_dev.cfg` was absent before the test and is absent now.
