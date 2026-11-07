# mproton

Experimental: Proton-style Windows game compatibility for Apple Silicon Macs (M1–M3).

- [docs/macos-port-plan.md](docs/macos-port-plan.md): research, architecture and milestones
- [docs/m0-baseline.md](docs/m0-baseline.md): milestone 0 baseline numbers
- [docs/steam-integration.md](docs/steam-integration.md): how macOS Steam's dormant Steam Play is enabled (NotProton, Kaon)
- `launcher/`: `mproton`, the macOS port of Proton's launcher script ([launcher/README.md](launcher/README.md)); tests in `tests/` (`python3 -m pytest tests`)
- [docs/wine-patch-triage.md](docs/wine-patch-triage.md): Proton's Wine fork vs the macOS ARM64 patch stacks (`wine-patches/`)
- `scripts/setup.sh`: fetch the toolchain (llvm-mingw) and the upstream Proton reference clone
- `steam/`: Proton's `lsteamclient` and `steam_helper` (`steam.exe`) ported to arm64 macOS, with a standalone Wine-SDK build and struct-layout (ABI) checks against the macOS Steamworks ABI ([steam/README.md](steam/README.md))
- `bench/`: mpbench, a synthetic CPU / D3D11 / D3D12 / raw Vulkan benchmark and feature probe built for x86_64/i686/aarch64/arm64ec, plus `compare.py` to tabulate results
- `scripts/validate-lavapipe.sh`: correctness-only run of mpbench on Linux (Wine + DXVK + vkd3d-proton on lavapipe)
