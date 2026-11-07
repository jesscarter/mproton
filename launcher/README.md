# mproton launcher

`mproton` is a port of Valve's `proton` launcher script (upstream `bc89621`, 2,189 lines) to macOS on Apple Silicon.

## Layout

| File | What it is |
|---|---|
| `mproton` | Upstream `proton` with a small set of edits. Every changed spot carries an `#mproton:` comment. View the full delta with `diff -u ref/proton/proton launcher/mproton` |
| `mproton_host.py` | Host-platform layer. All Linux-vs-Darwin decisions live here (`g_host`) |
| `mproton_graphics.py` | Graphics backend selection and the per-backend DLL placement plan (Proton dist) |
| `mproton_runtime.py` | Finds a mach9 bundle and knows its paths and Direct3D trees |
| `mproton_compatdb.py` | Turns backend choices and DLL overrides into mach9 compatdb rules |
| `steam/` | `compatibilitytool.vdf`, `toolmanifest.vdf` and the `run` entry point for macOS Steam. See [../docs/steam-integration.md](../docs/steam-integration.md) |

Dist layout: these files sit next to upstream's `filelock.py`, etc., in `compatibilitytools.d/mproton/`. The Wine runtime sits beside them in one of two forms:

- a mach9 bundle in `wine/`, or any bundle named by `MPROTON_WINE_DIR` (macOS); or
- Proton's `files/` tree (Linux, and macOS only when no bundle is found).

## mach9 bundle

On Darwin the launcher first looks for a mach9 bundle (athei/mach9 `dist/wine`), in this order:

1. `$MPROTON_WINE_DIR`: the bundle itself, or a directory that holds it as `wine/`
2. `<launcher dir>/wine`
3. `<launcher dir>/files`

A bundle is recognised by `bin/wine` plus `lib/wine/aarch64-unix/compatdb.so`. If `MPROTON_WINE_DIR` is set but doesn't point at a bundle, the launcher exits with an error and doesn't fall back. On Linux the variable is ignored, since the bundle is arm64 Mach-O. Running a game by hand is covered in [../docs/running.md](../docs/running.md).

| Area | Proton dist | mach9 bundle |
|---|---|---|
| Wine | `files/bin*/wine`, `wineserver` | `bin/wine` (symlink to the loader in `wine.app`), `bin/wineserver` |
| Prefix | copied from `files/share/default_pfx*`, builtins symlinked, tracked | `wine wineboot -i` once (`creation_sync_guard`), then `wineserver -w`. Wine updates the prefix itself when `wine.inf` changes. Version `mach9-1` |
| Direct3D | DLLs copied into `system32`/`syswow64` + `WINEDLLOVERRIDES=…=n` | nothing copied. Rules in `<compatdata>/compatdb.rules`, named by `WINE_COMPATDB_FILE`. `compatdb.so` prepends one tree per family (`lib/wine/dxgi/<impl>/`, `lib/wine/d3d9/<impl>/`) |
| DLL overrides | `WINEDLLOVERRIDES` | `dll_overrides=` entries in the `exe=*` rule. An inherited `WINEDLLOVERRIDES` is folded in first, then removed from the environment |
| Large address aware | `WINE_LARGE_ADDRESS_AWARE` | `large_address_aware=` |
| `WINEDLLPATH`, `GST_*`, media/espeak paths | set | not set: the bundle finds its own; a user's `WINEDLLPATH` is kept |
| Vulkan | n/a | `VK_DRIVER_FILES` not set: ntdll points it at the bundled KosmicKrisp (`share/vulkan/icd.d/`) unless the user exported a driver variable, which passes through |
| msync | `WINEMSYNC=1`, removed for `nomsync` | `WINEMSYNC=1` (or the user's value); `0` for `nomsync`/`nofsync`, since unset means **on** in mach9 |
| `WINENCPU`, `FEX_HARDWARE_TSO`, `FEX_*` | n/a | passed through; `PROTON_CPU_TOPOLOGY` / `default_cpu_limit` also set `WINENCPU` |
| `steam.exe` | `files/lib/wine/<arch>/steam.exe` via the default prefix | wineboot's placeholder, or a copy from the bundle's `lib/wine/{aarch64,i386}-windows/` (with `lsteamclient.dll`) when the prefix lacks it. Without `steam.exe` (or with `MPROTON_NO_STEAM_EXE=1`) the game runs directly as `bin/wine <game>` |
| `WINE_COMPATDB` | n/a | passed through, so users can add rules by hand |
| `WINE_PROF_SERVICE` | n/a | an inherited value is dropped; `MPROTON_PROF_SERVICE` sets it on purpose |

### Backends as compatdb rules

The rules use the format of the `compatdb-core` crate (<https://docs.mach9.app>, "Format"). `mproton_compatdb.py` writes it as the crate's `Display` does: fields in `Key::ALL` order, and `%`, `;` and control characters escaped as `%XX`. `tests/test_compatdb.py` pins that text.

| Backend (env / `STEAM_COMPAT_CONFIG` flag) | `exe=*` rule |
|---|---|
| default on a bundle (`mach9`) | no tree, so the built-in rules apply: `dxgi=dxmt` (D3D12 from vkd3d-proton in that tree), `d3d9=mtld3d` |
| `PROTON_USE_DXMT` / `dxmt` | `dxgi=dxmt` |
| `PROTON_USE_WINED3D`, `PROTON_USE_WINED3D11` / `wined3d`, `wined3d11` | `dxgi=wined3d;d3d9=wined3d`, which leaves no D3D12 |
| `PROTON_USE_DXVK` / `dxvk` | `dxgi=dxvk;d3d9=dxvk`, each only if `lib/wine/<family>/dxvk/aarch64-windows` exists |
| `PROTON_USE_D3DMETAL` / `d3dmetal` | `dxgi=d3dmetal`, only if `lib/wine/dxgi/d3dmetal/aarch64-windows` exists; otherwise it falls back like any other backend |
| `PROTON_USE_MTLD3D` / `mtld3d` | `d3d9=mtld3d` |
| `MPROTON_DXGI=<tree>`, `MPROTON_D3D9=<tree>` | that family's tree, or `default` |
| `PROTON_NO_D3D12` / `nod3d12` (both layouts) | `dll_overrides=d3d12=;dll_overrides=d3d12core=` |
| `forcelgadd` (on by default) / `noforcelgadd` | `large_address_aware=true` / `false` |
| `MPROTON_METAL_HUD=1` | `metal_hud=true` |

Further details:

- **Precedence** between backends is unchanged: wined3d > d3dmetal > dxmt > dxvk.
- **Missing trees.** A tree missing from the bundle is logged, and that family keeps the built-in tree.
- **Built-in program rules.** An `exe=*` rule doesn't beat a built-in rule that pins one program's tree, such as Steam's web helper or GTA IV. So when a backend is chosen explicitly and `SteamAppId` is set, a second rule `exe=steam:<appid>;dxgi=…;d3d9=…` repeats the choice.
- **DXVK and D3DMetal** are not values in upstream mach9's `Dxgi`/`D3d9` enums. They need a `compatdb.so` that knows them ([../docs/graphics-dxvk.md](../docs/graphics-dxvk.md#adding-dxvk-as-a-backend-in-mach9)). A stock `compatdb.so` ignores the field with a warning and keeps the built-in tree.
- **D3DMetal paths.** For D3DMetal, `PROTON_D3DMETAL_PATH` only applies to the Proton dist. On a bundle the GPTK modules have to be arranged as a `dxgi/d3dmetal` tree, because `compatdb.so` prepends its tree ahead of any `WINEDLLPATH`.
- **Rules file.** It is rewritten atomically on every launch, `runinprefix` included.

## Behaviour on Darwin (Proton dist)

| Area | Upstream (Linux) | mproton on Darwin |
|---|---|---|
| Runtime | pressure-vessel, `steam-runtime-launcher-interface-0`, `require_tool_appid` | none; SLR version logging skipped |
| Libraries | `LD_LIBRARY_PATH` + `ORIG_LD_LIBRARY_PATH`, Steam `ubuntu12_*/video` | no env var. Bundled dylibs are found via `LC_RPATH` (SIP strips `DYLD_*` through `/bin/sh` and system python; hardened runtime ignores them). `MPROTON_DYLD_FALLBACK=1` sets `DYLD_FALLBACK_LIBRARY_PATH` for unsigned dev builds |
| `/proc` | `/proc/modules` (NVIDIA probe), `/proc/sys/vm/max_map_count` | not read |
| Sync | `WINEFSYNC=1` unless `nofsync` | `WINEMSYNC=1` unless `nomsync` / `PROTON_NO_MSYNC=1` or the game's `nofsync` table entry; `WINEFSYNC`/`WINEESYNC`/`WINENTSYNC` cleared |
| FEX | `platform.machine() == 'aarch64'` | `g_host.is_arm64` (macOS reports `arm64`); `generate_fex_app_config` unchanged |
| casefold | ext4 `EXT2_IOC_SETFLAGS` | skipped; warns if `drive_c` sits on a case-sensitive APFS volume |
| copies | `copy_file_range` reflink | APFS `clonefile(2)` |
| launch | `wine-preloader` for x86_64 | always `bin*/wine` (no separate preloader on macOS) |
| nvapi, NVIDIA NGX, OpenVR/OpenXR | yes | skipped |

The per-game compat tables (`default_compat_config`, `default_cpu_limit`, `fex_application_profiles`, and the inline appid lists) are unchanged.

## Graphics backends (Proton dist)

| Select with | Backend | Where the DLLs come from |
|---|---|---|
| (default) | DXVK + vkd3d-proton | `files/lib/wine/{dxvk,vkd3d-proton}/<pe-arch>/`, as upstream |
| `PROTON_USE_DXMT=1` or compat flag `dxmt` | DXMT for D3D10/11/DXGI | `files/lib/wine/dxmt/<pe-arch>/{d3d11,d3d10core,dxgi,winemetal}.dll`; D3D9 stays on DXVK, D3D12 on vkd3d-proton |
| `PROTON_USE_WINED3D=1` or `wined3d` | wined3d | Wine builtins, as upstream |
| `PROTON_USE_D3DMETAL=1` + `PROTON_D3DMETAL_PATH=<GPTK>` | D3DMetal (bring your own) | nothing copied. The GPTK `lib/wine` dir is prepended to `WINEDLLPATH`, `d3d12/d3d11/d3d10/dxgi=b` |

Notes:

- If several backends are requested, precedence is wined3d > d3dmetal > dxmt > dxvk.
- Backends can also be chosen with `STEAM_COMPAT_CONFIG` (e.g. `dxmt`), and so through per-game tables.
- D3DMetal activates only if `PROTON_D3DMETAL_PATH` contains `external/D3DMetal.framework` **and** D3DMetal PE and unix files for the running Wine's arch. Otherwise it logs why and falls back. GPTK 2.x ships only x86_64 D3DMetal, so on arm64 Wine it will fall back until Apple ships an arm64 build.

## Tests

```bash
python3 -m pytest tests
```

The tests run on Linux. They patch `platform.system()`/`machine()`, build a fake dist tree with tagged PE files, and drive `Proton`/`CompatData`/`Session` the way `__main__` does. They cover:

- env construction
- DLL placement for each backend and host
- prefix creation, drives and cleanup
- the `run` argv
- on mocked mach9 bundles (`test_mach9.py`, `test_compatdb.py`): bundle lookup, backend → rules mapping and the exact rules text, env construction and pass-through, `wineboot -i` prefix creation, `steam.exe` placement, and the `run` argv with and without `steam.exe`
