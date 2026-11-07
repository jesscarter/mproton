# Milestone 0: baseline

Goal: get numbers for the existing ways of running Windows code on this Mac, so later mproton builds have something to beat.

Machine: MacBook Air, Apple M2 (4P+4E), 16 GB.

## Tooling (done)

- `scripts/setup.sh` downloads llvm-mingw (20261104, universal macOS) to `toolchains/` (gitignored).
- `bench/mpbench.cpp` is a small deterministic benchmark. `bench/build.sh` builds it for x86_64, i686, aarch64 and arm64ec into `bench/out/`.
  - CPU scenes: scalar FP, vectorisable FP, memcpy, locked atomics, and a plain-store message-passing ping-pong. The ping-pong's `ordering_errors` exposes broken TSO emulation.
  - GPU scenes (D3D11 FL 11_0): 4,000 draws per frame with a constant-buffer Map each, which measures translation overhead, plus a fullscreen ALU-heavy pixel shader.
  - Writes JSON to `--out`. Results go in `results/`.

## Results so far: macOS 14.0, Wine 10.0 (Gcenx "Wine Stable", x86_64 under Rosetta 2)

| scene | x86_64 | i686 (wow64) |
|---|---|---|
| cpu.scalar (Miter/s) | 160.3 | 159.9 |
| cpu.vector (GFLOP/s x2) | 8.0 | 7.2 |
| cpu.memcpy (GB/s) | 18.6 | 18.3 |
| cpu.atomic (Mops/s) | 59.3 | 57.0 |
| cpu.pingpong (K round-trips/s) | 12,213 (0 ordering errors) | 12,201 (0 errors) |
| gpu | **feature level 9_3 only** (wined3d over Apple GL 4.1, reports "NVIDIA GeForce 6800"). D3D11 SM5 is unusable | n/a |

Takeaway: plain Wine on macOS can't run D3D11-class games at all without DXVK, DXMT or D3DMetal. That's expected, and it confirms the graphics stack is the main work.

## Still to do (needs macOS 27.2, update pending)

1. Re-run the Rosetta baseline on 27.2. Rosetta still works on 27.
2. **Rosetta + DXVK/MoltenVK** and **Rosetta + DXMT**: Gcenx/CrossOver-style builds.
3. **ARM64 Wine + FEX**: CrossOver Preview (ARM64 build), MetalSharp-Wine or athei/mach9. Compare CPU numbers with Rosetta, check `ordering_errors` with hardware TSO on and off, and record which GPU layers work.
4. **KosmicKrisp**: build from Mesa (+ steamac patches) and run `vulkaninfo`. Then run DXVK on it under ARM64 Wine.
5. **Native aarch64 / arm64ec builds of mpbench** under ARM64 Wine: the ceiling with no x86 translation.
6. Optional: one real Steam game you own, as a sanity check alongside the synthetic numbers.
