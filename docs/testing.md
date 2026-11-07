# Testing

mproton has three kinds of tests:

| What | Where | Answers |
|---|---|---|
| Orchestration and launcher tests | `tests/*.py` (`python3 -m pytest tests`) | Do our scripts and the launcher do the right thing? These run on Linux with stubs and never start Wine. |
| Benchmark | `bench/mpbench`, `tools/run-matrix.sh` | How fast is it? |
| **Correctness ladder** | `tests/ladder/` | Does the Wine bundle run Windows code correctly? |

This page is about the ladder. The idea comes from the validation ladder that VKMT, a native-ARM64 Wine for macOS, describes.

## The ladder

Small Windows programs, each built for aarch64, arm64ec, x86_64 and i686. Each one prints one machine-readable line per check (`LADDER<TAB>PASS|FAIL|SKIP|INFO<TAB>program.check<TAB>detail`), then a `DONE` line. If the `DONE` line is missing, the program crashed or hung.

| Program | Checks | Catches |
|---|---|---|
| `threads` | static TLS (`thread_local`, the image's TLS directory and callbacks), `TlsAlloc` including expansion slots past 64, FLS destructors, critical sections, SRW locks and condition variables under contention, APCs (`QueueUserAPC` with `SleepEx`/`WaitForSingleObjectEx`, FIFO order, nothing during a non-alertable wait), waitable-timer completion routines, 500 short threads | Thread and TEB setup, and APC delivery through the FEX/WoW64 transitions |
| `seh` | access violations on read, write and execute, with the exact address, read/write flag and context PC/SP; integer divide by zero (`idiv` on x86, `brk #0xf003` on ARM64, like MSVC); breakpoints; nested handlers and their order; `RaiseException` through ten frames with `__finally` blocks; continuable exceptions; vectored handlers that fix a fault and continue; C++ throw/catch; stack overflow | Exception dispatch and unwinding. Under FEX, the context has to be rebuilt in x86 terms. i686 uses hand-built `fs:[0]` frame SEH, the way MSVC-built 32-bit games do. |
| `callbacks` | window procedures (direct, cross-thread `SendMessage`, subclassing), `EnumWindows`/`EnumChildWindows` including early stop, `SetTimer` with a `TIMERPROC`, `WH_CALLWNDPROC` hooks, timer queue, thread-pool work, registered waits, CRT `qsort`, `EnumFontFamiliesEx` | Wine (native ARM64 on the Mac) calling back into x86/x64 code |
| `memory` | 4K page size and 64K granularity; `VirtualProtect` on single 4K pages inside one 16K host page (access, `VirtualQuery` regions, contents kept); guard pages that fire once and only for their own 4K; per-4K commit/decommit; `GetWriteWatch` at 4K granularity; `VirtualAlloc2`/`MapViewOfFile3` placeholders; section aliasing; a 1 GB reservation. Self-modifying code: W^X cycles, RWX rewrites with and without `FlushInstructionCache` (without is legal on x86), data next to live code, free-and-reallocate at the same address, an RX page next to a hot RW page, and code rewritten on one thread while another runs it. | 16K-page emulation bugs and FEX JIT invalidation bugs |
| `ordering` | Litmus tests on plain loads and stores, 500k iterations each: message passing (barrier-synced and streaming), load buffering, IRIW, store buffering, read-read coherence, a spinlock released with a plain store, interlocked increments. Each test also runs with fences. | Missing TSO for x86 code. See below. |
| `d3d11` | device, clear, a fullscreen `SV_VertexID` pass, a vertex-buffer quad coloured from a constant buffer, `UpdateSubresource`, `CopyResource`, `CopySubresourceRegion` (texture and buffer), all read back through staging resources and compared pixel by pixel | wined3d, DXMT or DXVK, whichever compatdb selects |
| `d3d12` | fences (CPU signal, queue signal, a queue waiting on a CPU-signalled fence, 200 queued signals), clear plus a copy to a readback heap, two draws through root signatures and PSOs, an upload on a COPY queue consumed by the DIRECT queue after a cross-queue fence wait, all checked pixel by pixel | vkd3d-proton on KosmicKrisp |
| `d3d9` | adapters, identifier, display modes, HAL caps, format support, `Direct3DCreate9Ex`, then a device: clear and a fixed-function quad read back with `GetRenderTargetData` | mtld3d, wined3d or DXVK |
| `opengl` | WGL context, then into an FBO: clear, an immediate-mode quad, a GLSL pass, all checked with `glReadPixels`, plus `glGetError` | Zink on KosmicKrisp (EGL) or CGL |
| `vulkan` | instance, device, `vkCmdClearColorImage`, a buffer-to-image copy into a sub-rectangle with a row length, fill and copy buffers, 50 submissions reusing one fence | winevulkan to KosmicKrisp |

All colours are multiples of 1/255, so any correct float-to-UNORM conversion gives the same byte. Every pixel check is exact.

Graphics DLLs (D3D, DXGI, Vulkan) are loaded with `LoadLibrary`, so a missing DLL shows up as a `SKIP` instead of a program that won't start. For example, the wined3d tree has no `d3d12.dll`.

### Memory ordering

x86 code assumes TSO. FEX provides it in one of two ways:

- `FEX_HARDWARE_TSO=1`: the M-series per-thread TSO bit.
- `FEX_HARDWARE_TSO=0`: ordered loads and stores, plus barriers.

The ordering suite checks both. For x86 builds, every TSO-forbidden outcome has to stay at 0. Store buffering (`sb`) is allowed even under TSO, so it's reported as `INFO`. Its count is the calibration: a non-zero `sb` proves that the threads overlap, so a clean result in the other tests means something.

Native ARM builds (aarch64, arm64ec) run the same plain tests as `INFO`. ARM is allowed to reorder them, so seeing reorderings there shows the harness can catch them on that Mac. Fenced variants, coherence and atomics must be clean on every arch.

## Running it

Build the programs (llvm-mingw from `scripts/setup.sh`):

```bash
tests/ladder/build.sh
```

Run the whole ladder on the Mac bundle:

```bash
tests/ladder/run.sh work/mach9/dist/wine
```

`run.sh` writes `results/ladder-<date>/`, which contains:

- `summary.md`: the pass/fail matrix
- `checks.tsv`: every check
- `runs.tsv`: every program run
- `logs/`

It exits 0 only when every `FAIL` is listed in `tests/ladder/known-failures.txt`.

The rungs, in order:

1. **setup**: fresh prefix, then `wineboot -u`. The server has to exit on its own afterwards.
2. **serial**: every program for every arch.
   - Graphics programs run once per backend: each `lib/wine/dxgi/<impl>` tree (wined3d, dxmt, dxvk) for D3D11/D3D12, each `lib/wine/d3d9/<impl>` tree for D3D9, and `opengl=egl`/`cgl`. Each backend is selected with a `WINE_COMPATDB` rule.
   - The `compatdb:` block that each program's own process logs is checked. A run that got a different tree is a `FAIL`.
3. **concurrent**: all four archs at once, each running every program, in one wineserver lifetime. A sentinel process holds the server open and has to be alive at the end.
4. **tso**: the ordering suite again for x86_64 and i686, with `FEX_HARDWARE_TSO=1` and then `=0`.
5. **heavy** (optional, `LADDER_HEAVY="mono chromium"`):
   - **mono**: a C# program (`src/heavy/hello.cs`) on Wine Mono. It covers the JIT, null-reference and divide-by-zero faults becoming managed exceptions, threads, P/Invoke and the GC. It needs `mcs` or `csc`, and Wine Mono (`LADDER_WINE_MONO_MSI`).
   - **chromium**: a Windows Chromium snapshot (`LADDER_CHROMIUM=…/chrome.exe`), run headless with `--dump-dom` on a page whose JavaScript computes a checksum. This starts renderer, GPU and utility subprocesses and runs V8's JIT under FEX.
   - The real target behind the chromium rung is the Windows Steam client's `steamwebhelper.exe` (CEF). VKMT and MetalSharp both name Java, Chromium and Steam's WebHelper as the workloads that need TSO. The WebHelper needs a Steam login, so it isn't automated.
6. **shutdown**: `wineserver -w` has to return by itself, which means no process outlived the ladder. Then `wineserver -k`, after which the server socket must be gone and no ladder process may be left.

`run.sh`'s header lists the knobs: `LADDER_ARCHS`, `LADDER_PROGS`, `LADDER_GFX`, `LADDER_BACKENDS`, `LADDER_TIMEOUT`, `LADDER_PHASES` and others. Every program also takes `--only PREFIX` and `--timeout SECS`, and `ordering` takes `--iters N`. Each one runs by hand too:

```bash
WINEPREFIX=… bin/wine tests/ladder/out/memory-x86_64.exe --only smc
```

## Linux validation

The same ladder runs on Linux with the distro Wine on Mesa (llvmpipe for GL, lavapipe for Vulkan):

```bash
tests/ladder/run.sh system
```

On a headless box it starts Xvfb itself. The result on Ubuntu 24.04 (Wine 9.0, Mesa 25.2.8) is in [`results/ladder-2026-11-07-linux/summary.md`](../results/ladder-2026-11-07-linux/summary.md): every program passes for x86_64 and i686, serially and concurrently, including all exact-pixel checks on wined3d, Wine's vkd3d, Mesa GL and lavapipe.

The one exception is `seh.stack_overflow` on x86_64. Wine 9.0 kills the thread instead of dispatching `STATUS_STACK_OVERFLOW`, so it's listed in `known-failures.txt` for `wine-9.0*` only.

`tests/test_ladder.py` checks the orchestration itself (backends, compatdb mismatch detection, crashes, timeouts, the TSO phase, known failures) against a stub bundle.

What Linux **can't** validate, and needs the Mac:

- **aarch64 and arm64ec builds.** Nothing runs them on x86 Linux. They compile in every build, but their SEH, ARM64EC call checking (generated x64 code from arm64ec goes to the emulator) and `brk #0xf003` paths have only run on the Mac.
- **FEX**, so all of the following: x86 code translated to ARM64, the `FEX_HARDWARE_TSO=1/0` phase, JIT invalidation under self-modifying code, and SEH contexts rebuilt from emulated state. On Linux the x86 programs run natively, so the ordering suite only checks the harness: `sb` shows the threads overlap and nothing forbidden appears.
- **16K pages.** The x86 Linux host has 4K pages, so the 4K-in-16K protection, guard-page, write-watch and placeholder checks pass trivially here. They mean something only on Apple Silicon.
- **The Mac graphics stacks**: DXMT, vkd3d-proton on KosmicKrisp, mtld3d, Zink and CGL, and compatdb backend selection. On Linux the graphics programs exercise Wine's own wined3d/vkd3d on Mesa.
- **The heavy rungs.** `hello.cs` was compiled with Mono's `mcs` and run under host Mono, and the Chromium page's checksum was checked against Node. Running them under Wine needs Wine Mono and a Chromium snapshot, which weren't run in the Linux validation.
- **msync** (the Mac's sync primitives) and the bundle's signing/entitlements. The Linux Wine uses wineserver sync.
