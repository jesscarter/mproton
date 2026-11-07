# Steam components: lsteamclient and steam_helper

Proton's two Steam bridge components, built for arm64 macOS:

| Directory | Builds | What it does |
|---|---|---|
| `lsteamclient/` | `lsteamclient.dll` (PE) + `lsteamclient.so` (unix side) | The Windows `steamclient.dll`/`steamclient64.dll` API. Each call is thunked to the native Steam client library (`steamclient.dylib` on macOS). |
| `steam_helper/` | `steam.exe` (PE) | The stub `steam.exe` that the launcher runs the game under. It sets up Steam's registry keys and `libraryfolders.vdf`, then starts the game. |

## Provenance

Both are imported from upstream Proton, `github.com/ValveSoftware/Proton`, default branch at
`bc896217ffcd67f892f952b010f898fe7bd0f0a1` (2026-10-09). That is the same commit the launcher port
(`launcher/mproton`) is based on. The newest upstream commits touching each directory are:

- `lsteamclient/`: `3b36d24400f5` (2026-07-13), "lsteamclient: Avoid calling Unix side setenv / unsetenv when not needed."
- `steam_helper/`: `5b89db940e0e` (2026-09-03), "steam_helper: Also recognize steam2ea:// as EA launcher."

Use this to see the full local delta:

```bash
git clone https://github.com/ValveSoftware/Proton ref/proton && git -C ref/proton checkout bc896217ffcd
```

```bash
diff -ru -x Makefile.in ref/proton/lsteamclient steam/lsteamclient
```

Every hand-made change carries an `mproton:` comment. The SDK headers and generated files are unchanged.

Licenses:

- `lsteamclient/LICENSE` is the Steamworks SDK license. It covers the `steamworks_sdk_*` headers.
- `LICENSE.proton` is Proton's BSD license. It covers everything else.

### What changed from upstream

| File | Change |
|---|---|
| `Makefile.in` (both) | Removed. Proton builds these through its container and top-level `Makefile.in`. `Makefile` here replaces both. |
| `lsteamclient/gen_wrapper.py` | Explicit target triples per ABI instead of `-m32`/`-m64` on the host. Libc stubs in `gen_include/` instead of the host glibc (the i386 multilib is no longer needed, and the script runs on a macOS host). Clang resource dir auto-detected. **New `arm64-apple-macos` cross-check** (below). `--dump-layouts` for the ABI test. The generated output is byte-identical to upstream's. |
| `lsteamclient/unixlib.cpp` | macOS `steamclient.dylib` lookup (below). `dlerror()` is logged when `dlopen` fails. |
| `lsteamclient/unix_steam_input_manual.cpp` | macOS key codes for `ISteamHTMLSurface::KeyDown/KeyUp`. Upstream has `#error` for every non-Linux host, so the unix side did not build on macOS at all. |
| `steam_helper/steam.c` | Forwards `steam.exe` commands to the macOS client. Steam root vs bundle path in `libraryfolders.vdf`. No vrclient on macOS. |

## Build

You need:

- a Wine **source** tree and a configured and built Wine **build** tree from it, e.g. athei's `arm64-patched` built by mach9 with `--enable-archs=arm64ec,aarch64,i386`;
- llvm-mingw for the PE side;
- the host clang (Xcode) on macOS.

Proton's container is not used. Build with:

```bash
make -C steam WINE_SRC=/path/to/wine WINE_OBJ=/path/to/wine-build
```

Then install:

```bash
make -C steam install WINE_SRC=/path/to/wine WINE_OBJ=/path/to/wine-build DESTDIR=/path/to/dist/files
```

This produces, under `$(DESTDIR)/lib/wine/`:

| File | Format |
|---|---|
| `aarch64-windows/lsteamclient.dll` | ARM64X: aarch64 + arm64ec, for native arm64 and FEX/ARM64EC x86_64 games |
| `aarch64-windows/steam.exe` | aarch64. Wine never builds programs for arm64ec |
| `i386-windows/lsteamclient.dll`, `i386-windows/steam.exe` | i386, for WoW64 games. Their unix calls go through `__wine_unix_call_wow64_funcs` in the 64-bit unix side |
| `aarch64-unix/lsteamclient.so` | A Mach-O dylib named `.so`, like all of Wine's unixlibs. Linked with `-dynamiclib -install_name @rpath/lsteamclient.so` against `$(WINE_OBJ)/dlls/ntdll/ntdll.so` |

The Makefile issues the commands Wine's makedep generates for an in-tree module:

- `winegcc --wine-objdir … -Wl,--wine-builtin`;
- `-marm64x` to link aarch64 and arm64ec objects into one image;
- the same per-arch `-target`/warning flags and import libraries.

Compare against a DLL's rules in `$(WINE_OBJ)/Makefile` when Wine's flags change.

Useful variables:

- `PE_ARCHS` (default `aarch64 arm64ec i386`; `x86_64` also works)
- `UNIX_OS` (`darwin`/`linux`, from `uname`)
- `UNIX_ARCH`
- `UNIX_CC`, `UNIX_CXX`
- `aarch64_CC`, `arm64ec_CC`, `i386_CC`
- `NTDLL_SO` (set it empty to link the unix side without ntdll, for a cross build)
- `O` (build dir)

### Wine-side requirements

The game loads `steamclient.dll`/`steamclient64.dll` (Valve's), and the Wine loader has to redirect that to `lsteamclient.dll`. That redirect lives in Proton's Wine, not in this module. It comes from these commits (`wine-patches/triage/proton-commits.tsv`):

- `c177a34d784` *HACK: steam: ntdll: Setup steamclient trampolines to lsteamclient*
- `e8475167507` (Steam registry entries in `wine.inf`)
- `eeb10d71bea` (`steam://` protocol association)
- `adba6db13ee` (`C:\Program Files (x86)\Steam` on `PATH`)

The recommended Wine base (Proton Wine + athei's patches) carries these commits, and the mproton Wine series hand-merges `c177a34d784` onto athei 11.20. Plain athei `arm64-patched` does not have them. The modules build against either, because they use no Proton-only Wine API: every ntdll export the unix side imports and every PE import resolves against athei's tree.

**Open blocker on arm64 (not fixed here).** The redirect works through execute page faults on the trampolines. Proton only hooks `steamclient_handle_fault()` into `signal_x86_64.c` and `signal_i386.c`. On an arm64 host:

- `signal_arm64.c` has no such hook.
- The game's x86 code runs under FEX (ARM64EC, or the WoW64 BTCpu for 32-bit games), and that fault path doesn't reach the hook either.

So until the hook is ported, the redirect won't fire on macOS arm64. The port means either a hook in the arm64/ARM64EC fault path, or having FEX call it. Until then, games never reach lsteamclient.

## Linux-isms in the unix side (audit)

| Looked for | Found | macOS handling |
|---|---|---|
| `/proc`, ELF / `dl_iterate_phdr` / `link.h`, `getauxval`, `memfd`, `epoll`/`eventfd`, `prctl`, `syscall()`, `pthread_setname_np` | none in lsteamclient or steam_helper | n/a |
| `~/.steam/sdk{32,64,arm64}/steamclient.so` | `unixlib.cpp`, Linux branch only | `__APPLE__` branch, extended (below) |
| `<X11/keysym.h>`, VK→keysym table | `unix_steam_input_manual.cpp`, plus `#error` on other hosts | new VK → Carbon `kVK_*` table |
| `dlopen(…, RTLD_NOW)` | unixlib | portable. A universal dylib loads its arm64 slice in an arm64 process |
| `steam-runtime-steam-remote` / `steam` on `PATH` (forwarding `steam://` and `-options`) | `steam.c` | `/usr/bin/open` for `steam://` URLs. `-options` go to `Steam.AppBundle/Steam/Contents/MacOS/steam_osx`, falling back to `open -a Steam --args` |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` treated as the Steam root (library folder 0) | `steam.c` `setup_steam_files` | the `/Steam.AppBundle/Steam/Contents/MacOS` suffix (the launcher's fallback) is stripped to get `~/Library/Application Support/Steam` |
| `vrclient_x64` (OpenVR) | `steam.c` | skipped on Darwin |
| pressure-vessel / Steam Linux Runtime | none in these modules (the launcher already drops it) | n/a |

steam.exe detects the host with ntdll's `wine_get_host_version`, so the same PE binary works on Linux and macOS.

### Finding `steamclient.dylib`

`find_steamclient_dylib()` in `unixlib.cpp` tries these in order:

1. `$MPROTON_STEAMCLIENT_PATH`, an explicit override for testing.
2. `$STEAM_COMPAT_CLIENT_INSTALL_PATH/steamclient.dylib`. This matches what the mproton launcher sets: `…/Steam.AppBundle/Steam/Contents/MacOS`.
3. `$STEAM_COMPAT_CLIENT_INSTALL_PATH/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib`, in case Steam itself sets the variable to the Steam root, as Linux Steam does.
4. `$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib`.
5. Bare `steamclient.dylib` (DYLD search), as upstream's fallback.

## ABI: Windows Steamworks structs vs the macOS steamclient.dylib

The unix side passes structs to the native library using the `u64` layouts that `gen_wrapper.py` computed. Upstream computes them for x86_64 Linux. The question was whether arm64 macOS lays them out the same way.

**What the SDK headers say.**

- From SDK 1.28 on, the callback-packing switch is `#if defined(__linux__) || defined(__APPLE__)` → `VALVE_CALLBACK_PACK_SMALL` → `#pragma pack(push, 4)`. Windows gets `VALVE_CALLBACK_PACK_LARGE` (pack 8). This is in `isteamclient.h`, and from about 1.50 in `steamclientpublic.h`.
- `steamtypes.h` defines `POSIX` for any `__GNUC__` compiler, and `CSteamID`/`CGameID` sit inside `#pragma pack(push, 1)` on every platform.
- So macOS takes exactly the Linux branch. No SDK version has an `__APPLE__`-only packing difference.
- What remains is the target C ABI. Both are LP64, with 8-byte `uint64`/`double`/pointers, 4-byte `wchar_t` and enums. Arm64 Darwin's 8-byte `long double` would matter, but the SDK never uses it.

**What the tools say.** The generator was not changed, because nothing differs:

- `gen_wrapper.py` now parses every SDK a second time for `arm64-apple-macos11` and compares each struct and union against the `x86_64-linux-gnu` parse, recursively and including anonymous members. Result: **15,972 struct definitions in 91 SDK versions are identical.** If a future SDK differs, generation stops with the list, and a separate darwin ABI would have to be generated for those structs.
- Pointing the check at `i686-apple-macos` makes it fail, as it should.

**Test.** `steam/tests/check_abi.py`, run under pytest as `tests/test_lsteamclient_abi.py`, does three things:

1. It reruns the generator and checks the output is byte-identical to the committed sources.
2. It compiles `static_assert(sizeof/alignof/offsetof)` checks from the generator's layouts against the real SDK headers with `clang -target … -fsyntax-only`. No macOS SDK is needed; the libc stubs are in `gen_include/`. Each target gets 105,174 assertions across all 91 SDKs, and all pass:
   - **u64 on `arm64-apple-macos11`**
   - **w64 on `x86_64-`, `aarch64-` and `arm64ec-pc-windows-msvc`**: arm64ec code inside x86_64 games sees the game's own layouts
   - **w32 on `i686-pc-windows-msvc`**
3. It diff-checks x86_64-windows against arm64-darwin. 2,852 struct versions (67 distinct structs, mostly callbacks with `uint64`/`double` members after pack 4 vs 8) differ, and **every one has a generated w64↔u64 converter**.

Requirements: `pip install libclang==15.0.6.1` (upstream generates with libclang 15; 18 changes token extents and breaks `callconv()`) and any clang ≥ 15 driver. The test is skipped when they are missing.

Not covered by the layout check:

- **C++ vtable ABI.** The unix side calls `steamclient.dylib`'s interfaces through `u_ISteam*` vtables. Both platforms use the Itanium C++ ABI with the same declaration order, so slot order matches. Struct-return conventions differ (x8 on arm64 vs hidden pointer on x86_64), but the compiler handles that because the unix side is compiled natively against the same declarations. This is consistent by construction, not tested against the real dylib.
- **Function pointers handed to Steam** (`SetWarningMessageHook` and friends). Generated as `U_CDECL`, i.e. `__attribute__((sysv_abi))`, which aarch64 ignores. That is correct on arm64, where there is only one C calling convention.

## steam.exe and the mproton launcher

The launcher runs `wine c:\windows\system32\steam.exe <game> <args>` with `WINEDLLOVERRIDES=steam.exe=b` (`launcher/mproton` `Session.run`). Wine loads the builtin from `files/lib/wine/aarch64-windows/steam.exe`, so `make install DESTDIR=<dist>/files` is all the launcher needs.

`steam.exe` does the following:

- When `SteamGameId` is set, it creates Steam's `Steam3Master_SharedMemLock`/`Valve_SteamIPC_Class` objects and the hidden `vguiPopupWindow`.
- It calls `lsteamclient`'s `steamclient_init_registry`, which fills `HKCU\Software\Valve\Steam` from the running client. This is the first `dlopen` of `steamclient.dylib`.
- It writes `C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf`.
- It starts the game with `CreateProcess`/`ShellExecute`, waits for it, and honours `steam://rungameid/<id>` restarts.
- Nothing in that path uses pressure-vessel or Linux paths.

## Must verify on a Mac

Prerequisite: the steamclient trampoline redirect must be ported to the arm64/FEX fault path (see "Wine-side requirements"). Until then only items 1–3 can be tested; items 2–3 use the standalone probe and need no Wine.

Nothing here has run on macOS yet. Everything above was built and checked on Linux:

- the PE side with llvm-mingw against athei's tree;
- the unix side for aarch64-linux;
- the new `__APPLE__` branches syntax-checked with `__APPLE__` forced;
- ABI via clang's darwin target.

On an M1–M3 Mac, check:

1. **Build.** Run `make` against the mach9 Wine build. Check that the unix side compiles and links with Xcode clang/libc++. It was only syntax-checked with `__APPLE__` forced, on glibc/libstdc++.
2. **`dlopen` of the real `steamclient.dylib`.** `steam/tests/steamclient_probe.cpp` covers most of this item and item 3 without Wine. Build it with `clang++ -std=c++17 -o steamclient_probe steam/tests/steamclient_probe.cpp`, then run `./steamclient_probe --connect` with Steam running. It uses the same search order, `dlopen`s the dylib, resolves the 7 entry points, opens a pipe and user, and calls `ISteamUtils009`. Then check:
   - Check which path `find_steamclient_dylib()` takes, and what `STEAM_COMPAT_CLIENT_INSTALL_PATH` Steam really passes (`WINEDEBUG=+steamclient`).
   - Run `otool -L` on the dylib. Its dependencies must resolve via `@loader_path`/`@rpath`; `@executable_path` would point at Wine.
   - Code signing: Valve's signed dylib loaded into an ad-hoc-signed or unsigned Wine. If Wine is built with hardened runtime and no `disable-library-validation`, `dlopen` fails, and the logged `dlerror()` will say so.
   - Check that it exports all seven entry points `LOAD_FUNC` needs: `CreateInterface`, `Steam_BGetCallback`, `Steam_GetAPICallResult`, `Steam_FreeLastCallback`, `Steam_ReleaseThreadLocalMemory`, `Steam_IsKnownInterface`, `Steam_NotifyMissingInterface`.
3. **Connection.** `steamclient_init_registry` connects (`CreateSteamPipe`/`ConnectToGlobalUser` with `SteamAppId` set), and the UI language and app keys appear in the prefix's registry.
4. **Achievements and stats.** Use a game with achievements (`ISteamUserStats` `RequestCurrentStats` → `UserStatsReceived_t` → `SetAchievement`/`StoreStats`). This exercises callbacks and the w64↔u64 converters end to end. Test one x86_64 (ARM64EC/FEX) game and one 32-bit (WoW64) game.
5. **Overlay.** It is expected not to work at first. On Linux, the overlay is `gameoverlayrenderer.so` preloaded into the game. The macOS client injects `gameoverlayrenderer.dylib` into native macOS games and would need to hook Wine's Metal/MoltenVK/KosmicKrisp presentation. Record what happens with Shift+Tab, and whether the overlay notification for an unlocked achievement shows.
6. **`steam.exe` forwarding:**
   - `steam://` URLs from inside a game (e.g. `steam://store/<id>`) open in the running client via `/usr/bin/open`.
   - `-applaunch`-style options reach `steam_osx` and are forwarded to the running instance rather than starting a second client.
7. **Steam Input glyphs / HTML surface.** Glyph paths come back as unix paths and are converted. The `ISteamHTMLSurface` key codes use the new `kVK_*` table.
8. **Cloud saves** with the `mproton` tool name (see `docs/steam-integration.md`).

## Regenerating the wrappers

```bash
cd steam/lsteamclient && ./gen.sh
```

Add `--no-darwin-check` to `gen_wrapper.py` to skip the arm64-apple-macos comparison.
