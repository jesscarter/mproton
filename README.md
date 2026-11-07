# mproton

Experimental: Proton-style Windows game compatibility for Apple Silicon Macs (M1–M3).

- [docs/macos-port-plan.md](docs/macos-port-plan.md): research, architecture and milestones
- [docs/m0-baseline.md](docs/m0-baseline.md): milestone 0 baseline numbers
- [docs/steam-integration.md](docs/steam-integration.md): how macOS Steam's dormant Steam Play is enabled (NotProton, Kaon)
- `launcher/`: `mproton`, the macOS port of Proton's launcher script ([launcher/README.md](launcher/README.md)); tests in `tests/` (`python3 -m pytest tests`)
- `scripts/setup.sh`: fetch the toolchain (llvm-mingw) and the upstream Proton reference clone
- `bench/`: mpbench, a synthetic CPU/D3D11 benchmark built for x86_64/i686/aarch64/arm64ec
