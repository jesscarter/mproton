# mproton

Experimental: Proton-style Windows game compatibility for Apple Silicon Macs (M1–M3).

- [docs/macos-port-plan.md](docs/macos-port-plan.md): research, architecture and milestones
- [docs/m0-baseline.md](docs/m0-baseline.md): milestone 0 baseline numbers
- [docs/steam-integration.md](docs/steam-integration.md): how macOS Steam's dormant Steam Play is enabled (NotProton, Kaon)
- `launcher/`: `mproton`, the macOS port of Proton's launcher script ([launcher/README.md](launcher/README.md)); tests in `tests/` (`python3 -m pytest tests`)
- [docs/wine-patch-triage.md](docs/wine-patch-triage.md): Proton's Wine fork vs the macOS ARM64 patch stacks (`wine-patches/`)
- [docs/wine-tree.md](docs/wine-tree.md): the mproton Wine tree, Proton's Wine patches on athei's arm64 Wine (`wine/`: patch series, `apply.sh`, per-commit status)
- [docs/building.md](docs/building.md): `build/mproton-build.sh` builds the whole Wine bundle on the Mac (mach9 + Proton Wine series + DXVK + lsteamclient)
- `scripts/setup.sh`: fetch the toolchain (llvm-mingw) and the upstream Proton reference clone
- `steam/`: Proton's `lsteamclient` and `steam_helper` (`steam.exe`) ported to arm64 macOS, with a standalone Wine-SDK build and struct-layout (ABI) checks against the macOS Steamworks ABI ([steam/README.md](steam/README.md))
- `bench/`: mpbench, a synthetic CPU / D3D11 / D3D12 / raw Vulkan benchmark and feature probe built for x86_64/i686/aarch64/arm64ec, plus `compare.py` to tabulate results
- `graphics/dxvk/`: reproducible DXVK build (ARM64X + i386, llvm-mingw) and KosmicKrisp audit ([docs/graphics-dxvk.md](docs/graphics-dxvk.md))
- `tests/ladder/`: the correctness ladder, small Windows programs (threads/TLS/APC, SEH, callbacks, 16K-page memory and self-modifying code, memory-ordering litmus tests, D3D9/11/12, OpenGL, Vulkan with exact pixel checks) run by `tests/ladder/run.sh BUNDLE_DIR` ([docs/testing.md](docs/testing.md))
- `scripts/validate-lavapipe.sh`: correctness-only run of mpbench on Linux (Wine + DXVK + vkd3d-proton on lavapipe)
