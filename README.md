# mproton

Experimental: Proton-style Windows game compatibility for Apple Silicon Macs (M1–M3).

- [docs/macos-port-plan.md](docs/macos-port-plan.md): research, architecture and milestones
- [docs/m0-baseline.md](docs/m0-baseline.md): milestone 0 baseline numbers
- `scripts/setup.sh`: fetch the toolchain (llvm-mingw) and the upstream Proton reference clone
- `bench/`: mpbench, a synthetic CPU / D3D11 / D3D12 / raw Vulkan benchmark and feature probe built for x86_64/i686/aarch64/arm64ec, plus `compare.py` to tabulate results
- `scripts/validate-lavapipe.sh`: correctness-only run of mpbench on Linux (Wine + DXVK + vkd3d-proton on lavapipe)
