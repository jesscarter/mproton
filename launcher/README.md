# mproton launcher

`mproton` is a port of Valve's `proton` launcher script (upstream `bc89621`, 2,189 lines) to macOS on Apple Silicon.

## Layout

| File | What it is |
|---|---|
| `mproton` | Upstream `proton` with a small set of edits. Every changed spot carries an `#mproton:` comment. View the full delta with `diff -u ref/proton/proton launcher/mproton` |
| `mproton_host.py` | Host-platform layer. All Linux-vs-Darwin decisions live here (`g_host`) |
| `mproton_graphics.py` | Graphics backend selection and the per-backend DLL placement plan |
| `steam/` | `compatibilitytool.vdf`, `toolmanifest.vdf` and the `run` entry point for macOS Steam. See [../docs/steam-integration.md](../docs/steam-integration.md) |

Dist layout: these files sit next to upstream's `filelock.py`, `files/`, etc., in `compatibilitytools.d/mproton/`.

## Behaviour on Darwin

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

## Graphics backends

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
