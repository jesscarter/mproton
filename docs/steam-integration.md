# Steam integration on macOS

Status: research notes plus the tool files in `launcher/steam/`. Nothing here has been run against a real macOS Steam client from this repo yet; see "Must verify on a Mac" at the end.

Sources read (2026-11-07):

- **NotProton**, `github.com/NotProtonNot/NotProton`, commit `489c8d07ce863b80495c6dfbae855f2b628df4b0`
- **Kaon**, `github.com/natbro/kaon`, commit `2c5f864df3a1686cff3ca994f433e4b1d96e1a82`
- **Upstream Proton**, `ref/proton` at `bc89621`

Statements marked *(source)* are taken from those trees. Statements marked *(inference)* are ours.

## The problem

The macOS Steam client contains the Steam Play / `CCompatManager` code but leaves it inert. Two consequences follow:

- A `compatibilitytool.vdf` dropped into `compatibilitytools.d` is not enough on its own. Kaon's README says: "the additional Steam Play user-interface enablement and launch-redirection logic is not functioning, so compatibility tools on macOS are effectively ignored" *(source: kaon/README.md)*.
- Windows-only games show no Install button, and games with both builds always install the macOS depot.

Two community approaches exist, and they work very differently.

## Mechanism A: Kaon, platform override ConVar (no patching)

**Exactly what it does** *(source: kaon/README.md)*:

1. Write one line to `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_dev.cfg`:
   ```
   @sSteamCmdForcePlatformType windows
   ```
2. Restart Steam.

**What this does:**

- The whole client then behaves as a Windows client: it shows Install/Play for Windows games and **downloads only Windows depots**.
- It is global. Games with both builds "will eventually be updated or cleared to install just the Windows version".

**Where it does not work:**

- `steam.cfg`: per the README, "`@sSteamCmdForcePlatformType` is not an allowed ConVar in `steam.cfg` and will be ignored".
- The command line: `open /Applications/Steam.app --args -console "+@sSteamCmdForcePlatformType windows"` sets the ConVar, but too late for the Library UI.
- The runtime console (`steam://nav/console`, then `@sSteamCmdForcePlatformType windows` and `app_install <appid>`) works for manual installs only.

**Steam Play is not enabled.** Kaon launches games by editing `appcache/appinfo.vdf` (`steammetadataeditor`) to add a launch option that runs `../Kaon/launch_crossover.sh <bottle> <exe>`. No `%verb%` and no `STEAM_COMPAT_*` variables are involved, and the compat-tool path is never used.

**Caveats:**

- *(source)* `steam_dev.cfg` implies `-skipinitialbootstrap`, so the client stops self-updating while the file exists.
- *(source)* The updater can delete Windows installs of games that also have a macOS depot whenever Steam runs without the override.

**Relevance to mproton:** this is the zero-patching way to get **Windows depots**. It cannot route launches through `mproton %verb%`, so it can serve only as a depot-download fallback, e.g. paired with `mproton runinprefix` launched by hand. The same ConVar is the documented `steamcmd` route: `steamcmd +@sSteamCmdForcePlatformType windows +app_update <appid>`.

## Mechanism B: NotProton, dylib injection that wakes `CCompatManager`

This is the only known way to get real per-game Steam Play on macOS. All of the following is *(source)*.

### 1. Injection

`app/Sources/NotProtonApp/Model/SteamBundle.swift`, `SteamInstaller.swift`:

1. Copy `notproton.dylib` into `/Applications/Steam.app/Contents/MacOS/`.
2. Add `DYLD_INSERT_LIBRARIES` (and `SDL_JOYSTICK_BLACKLIST_DEVICES=0x05ac/0x0004`) under `LSEnvironment` in `Steam.app/Contents/Info.plist`. The original is backed up as `Info.plist.before-notproton`.
3. Re-sign ad hoc with `codesign -f -s -` on the dylib, on `Contents/MacOS/steam_osx`, and on the app. The ad-hoc signature removes hardened-runtime library validation, so the inserted dylib loads.
4. Optionally block client updates by writing `BootStrapperInhibitUpdateOnLaunch=enable` to `~/Library/Application Support/Steam/steam.cfg` and to the copy inside `Steam.AppBundle/Steam`. Updates would otherwise replace the binary and invalidate the signatures below.

NotProton does **not** use `steam_dev.cfg` or `@sSteamCmdForcePlatformType` (grep confirms no occurrences).

### 2. Locating functions

The hooks find their targets through per-build AOB/string-anchor signatures in `signatures/macos.arm64/<steam build>.json`. Builds 1788400362, 1789086785 and 1790904859 are in the tree; the README names stable 1792618285 and beta 1792626604. Only arm64 builds are covered. Examples:

- `CCompatManager::Init` is anchored on the string `STEAM_COMPAT_TOOL_MAPPINGS`.
- `SetCompatToolMapping` is anchored on `Software\\Valve\\Steam\\CompatToolMapping`.

### 3. The force-enable

`dylib/feats/compat.c`, called from hooks on `CCompatManager::Init` and `BIsCompatibilityToolEnabled` (`dylib/hooks/hook_compat.c`):

```c
#define COMPAT_MANAGER_ENABLED_OFF  0x7B0
void np_compat_force_enable(void *compat_mgr) { ... base[off] = 1; ... }
```

The function sets the manager's "enabled" byte. The offset is re-probed at runtime by `np_compat_probe_enabled_off`, starting from `0x7B0`. In addition, `dylib/feats/webpatch.c` patches the Steam web UI's JS so that the Steam Play settings and the per-game **Compatibility** tab are shown.

### 4. Tool registration

There are two parts to this:

- **Files.** NotProton writes these into `~/Library/Application Support/Steam/compatibilitytools.d/<name>/`, and sets `STEAM_EXTRA_COMPAT_TOOLS_PATHS` to that directory:
  ```
  "manifest"
  {
    "version" "2"
    "commandline" "/run %verb%"
  }
  ```
  ```
  "compatibilitytools"
  {
    "compat_tools"
    {
      "<name>"
      {
        "install_path" "."
        "display_name" "<display>"
        "from_oslist" "windows"
        "to_oslist" "macos"
      }
    }
  }
  ```
- **In-memory registration.** The file scanner alone is not enough. NotProton also calls `CCompatManager::YldRegisterTool` with `from_oslist="windows"`, `to_oslist="macos"`, `appid=0` and `COMPAT_TOOL_FLAG_NO_APP`. From the code comment: "to_oslist=macos passes the registration gate, from_oslist=windows intersects the app mask so the dropdown keeps it, and appid 0 marks a manager-local tool the resolver routes to the local run script."

Two more details:

- A `ResolveCompatToolForApp.local_redirect` prehook rewrites any tool with a non-zero appid (Valve's Proton, which has no macOS depot) to a local tool that runs `/run %verb%`.
- Per a code comment, the client applies Steam Cloud path mapping only when the tool's name contains `"proton"`. This is why our tool is called `mproton`.

### 5. Selecting the Windows depot

- A prehook on `CCompatManager::GetOSListOverrideForApp` (`oslist_gate`) forces the override on. The install platform then comes from the tool's `from_oslist` (`windows`). From the code comment: "Proton converts is only for Linux, so the oslist match fails on macOS and the app shows 'Install on Windows'".
- A hook on `FindToolForTargetApp` gives Windows-only apps with no mapping a default tool. It is gated by `GetValidPlatforms` being `WINDOWS && !MACOS`.
- Per-game choices go through Steam's own `SetCompatToolMapping`, stored under `CompatToolMapping`: priority 250 for a single app, 75 for the global default (appid 0).
- For games that ship **both** builds, the macOS depot is used unless the user ticks *Properties → Compatibility → Force the use of a specific Steam Play compatibility tool*. Per the README, this "will make Steam download the Windows version".

### 6. What the tool receives

From `dylib/feats/compat_run.sh`, the embedded `run` script:

- **Verbs.** `$1` is the verb:
  - `waitforexitandrun` for games
  - `run` for helpers such as install scripts
  - `getcompatpath`
- **Environment.** The script reads `STEAM_COMPAT_DATA_PATH`, `STEAM_COMPAT_INSTALL_PATH`, `STEAM_COMPAT_APP_ID`, `STEAM_COMPAT_LIBRARY_PATHS`, `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `SteamAppId` and `SteamGameId`.
- **Empty `STEAM_COMPAT_CLIENT_INSTALL_PATH`.** It falls back to `$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS`, which is also where `steamclient.dylib` lives.
- **Unset app IDs.** Steam leaves `SteamAppId` and `SteamGameId` unset for helpers.
- **Launch options.** `hook_launch.c` hooks `V_ParseShellCommandLinePOSIX` so the tool command line runs via `/bin/sh -c`. This makes `VAR=x %command%` launch options behave as they do on Linux.

## What mproton ships (`launcher/steam/`)

Install by copying the dist directory to `~/Library/Application Support/Steam/compatibilitytools.d/mproton/`. It holds:

- `mproton`, `mproton_host.py`, `mproton_graphics.py`, `mproton_runtime.py`, `mproton_compatdb.py` and `filelock.py`;
- the Wine runtime: a mach9 bundle as `wine/` (or set `MPROTON_WINE_DIR`), or a Proton-style `files/`;
- the three files below.

To run a game by hand on a mach9 bundle, without Steam's compatibility-tool path, see [running.md](running.md).

| File | Difference from upstream | Why |
|---|---|---|
| `compatibilitytool.vdf` | `to_oslist` is `macos` (upstream: `linux`); tool name `mproton` | The registration gate matches the host OS; the name must contain "proton" for Cloud path mapping |
| `toolmanifest.vdf` | no `require_tool_appid`; `commandline` is `/run %verb%` | `require_tool_appid` pulls in the Steam Linux Runtime. `/run` matches what NotProton's resolver routes local tools to |
| `run` | new | `/usr/bin/python3` on macOS is a Command Line Tools stub, so `run` picks a Python: bundled `files/python`, then `$MPROTON_PYTHON`, then `python3` on `PATH`. It then execs `mproton "$@"` |

On the launcher side (`launcher/mproton`):

- If `STEAM_COMPAT_CLIENT_INSTALL_PATH` is unset on Darwin, the launcher fills in the same fallback NotProton uses. lsteamclient's `find_steamclient_dylib()` (`steam/lsteamclient/unixlib.cpp`) then finds `steamclient.dylib`. It also accepts the Steam root, in case Steam itself sets the variable that way. lsteamclient and `steam.exe` live in `steam/`; see [steam/README.md](../steam/README.md).
- The launcher handles all upstream verbs, including `getcompatpath` via `winepath`.

**Recommended setup today** *(inference)*:

1. Use NotProton's injector, or an equivalent we maintain, to enable `CCompatManager`. Our `compatibilitytool.vdf` then appears as an additional tool, because NotProton exports `compatibilitytools.d` via `STEAM_EXTRA_COMPAT_TOOLS_PATHS`.
2. Select **mproton** per game in the Compatibility tab. This also forces the Windows depot for dual-platform games.

Without an injector, only Kaon's `steam_dev.cfg` route is available. It gives Windows depots, but launches must be done by hand.

## Risks

- **Client updates.** NotProton's hooks are tied to specific Steam builds, and an update can break them silently. Mitigation: `BootStrapperInhibitUpdateOnLaunch`, plus checking the build number before enabling.
- **Re-signing Steam.app ad hoc** *(inference)*:
  - Gatekeeper and the Steam bootstrapper may "repair" the bundle.
  - Anything that relies on Steam's Team ID may behave differently (Keychain items, the overlay injector).
- **No anti-cheat** *(source: NotProton README)*: VAC is not supported, and the port plan already excludes kernel anti-cheat.
- **`legacycompat/`** *(inference)*: upstream copies `steamclient.dll`, `steamclient64.dll`, `Steam.dll` and `SteamService.exe` from `$STEAM_COMPAT_CLIENT_INSTALL_PATH/legacycompat/`. It is unknown whether the macOS client ships that directory. The launcher tolerates its absence (`os.path.isfile` checks), but games that load `steamclient.dll` directly will need those Windows binaries from somewhere.

## Must verify on a Mac

1. With NotProton active, does Steam list `mproton` from our `compatibilitytool.vdf`, or does it only see tools NotProton registers in memory (`~/Library/Application Support/notproton/tools`)?
2. Is `commandline "/run %verb%"` honoured for a file-registered tool with `use_sessions`/`compatmanager_layer_name` set? Or do those Linux-only keys make the macOS parser reject the manifest?
3. Which variables does Steam set when it launches the tool on macOS? Is `STEAM_COMPAT_CLIENT_INSTALL_PATH` the Steam root or `Steam.AppBundle/Steam/Contents/MacOS`, and where does `steamclient.dylib` actually sit?
4. Does `legacycompat/` exist in the macOS client?
5. Does the Cloud path mapping work for the name `mproton`?
6. Does Kaon's `steam_dev.cfg` ConVar still work on the current client?
