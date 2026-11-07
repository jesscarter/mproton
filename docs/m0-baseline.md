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

## macOS 27.2 (updated 2026-11-06)

### Rosetta, re-run

Rosetta still runs Wine 10 on 27.2, with no "App Update Required" prompt. Results are in `results/m0-macos27-*.json`.

| scene | x86_64 on 14.0 | x86_64 on 27.2 | i686 on 27.2 |
|---|---|---|---|
| cpu.scalar (Miter/s) | 160.3 | 157.7 | 156.8 |
| cpu.vector (GFLOP/s x2) | 8.0 | 7.8 | 6.7 |
| cpu.memcpy (GB/s) | 18.6 | 16.3 | 15.2 |
| cpu.atomic (Mops/s) | 59.3 | 56.8 | 54.0 |
| cpu.pingpong (K round-trips/s) | 12,213 | 11,882 | 10,676 |

GPU is unchanged: feature level 9_3 only.

### The new macOS APIs (`probes/macos_compat_probe.c`)

- `posix_spawnattr_set_4k_page_size_np` and `thread_set_x86_64_compat` are exported from `libsystem_kernel` on 27.2. `posix_spawnattr_get_4k_page_size_np`, `thread_get_x86_64_compat` and `os_arch_x18_custom_abi` are not.
- **Without the entitlement:**
  - `thread_set_x86_64_compat(self, 1)` returns `KERN_FAILURE` (5).
  - Setting the 4K attribute succeeds, but `posix_spawn` then fails with `EBADMACHO` (88, "Malformed Mach-o file").
- `hw.optional.arm.FEAT_AFP` = 0 on M2.

### Signing is a hard requirement

athei/mach9's loader refuses to start without `com.apple.developer.cross-architecture-support`. That entitlement only takes effect through a provisioning profile embedded in a `.app` and a signature from that profile's team. There is no 16K-page fallback. So **any** ARM64 Wine we build ourselves needs our own Apple Developer team, provisioning profile and signing identity.

- **Decision (2026-11-06):** enroll in the paid Apple Developer Program. Enrollment is in progress.
- **CrossOver Preview** (ARM64 + FEX) is only available to holders of a paid CrossOver license, so we aren't using it.
- **Full Xcode** (not just the Command Line Tools) is needed for the Metal toolchain (`xcrun metal`, used for DXMT's shaders) and for managing the profile.

### ARM64 build from source (in progress)

athei/mach9 checks out its sources into `work/mach9/` (gitignored):
- `scripts` = mach9
- `src` = athei/wine `arm64-patched`
- FEX, Mesa (KosmicKrisp), DXMT and vkd3d-proton are cloned by the build itself.

The command:

```bash
cd work/mach9 && MINGW_DIR=$PWD/../../toolchains/llvm-mingw scripts/bin/build-wine.sh
```

Then `scripts/bin/bundle-wine.sh`, which needs the signing identity.

## Still to do

1. **Rosetta + DXVK/MoltenVK** and **Rosetta + DXMT**: Gcenx/CrossOver-style builds.
2. **ARM64 Wine + FEX** (our own mach9 build, once signed): compare CPU numbers with Rosetta, check `ordering_errors` with `FEX_HARDWARE_TSO=1` and `=0`, and record which GPU layers work.
3. **KosmicKrisp**: build from Mesa (+ steamac patches) and run `vulkaninfo`. Then run DXVK on it under ARM64 Wine.
4. **Native aarch64 / arm64ec builds of mpbench** under ARM64 Wine: the ceiling with no x86 translation.
5. Optional: one real Steam game you own, as a sanity check alongside the synthetic numbers.
