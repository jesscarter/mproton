# DXVK for mproton

Workstream 3 (graphics) of [macos-port-plan.md](macos-port-plan.md): D3D8/9/10/11 go through
DXVK on KosmicKrisp. This page covers the build, an audit of DXVK against KosmicKrisp's
feature list, the `dxvk.conf`, the (empty) patch set, how DXVK would become a backend tree in
athei's mach9 bundle, and the Linux smoke test.

| | |
|---|---|
| DXVK pin | `v3.1.1` = `b1a1c99ab52b687cf950d62c88bc2fa316b41663` (2026-09-15), in [`graphics/dxvk/dxvk.pin`](../graphics/dxvk/dxvk.pin) |
| Toolchain | llvm-mingw 20261104 (clang 23.1.5), from `scripts/setup.sh` |
| 64-bit | **ARM64X** (ARM64EC + ARM64 halves), `aarch64-windows/` |
| 32-bit | i386, `i386-windows/` |
| DLLs | `d3d8 d3d9 d3d10core d3d11 dxgi` |
| Hard requirements KosmicKrisp misses | **none** (53 of 53 present) |
| Patches | none needed |
| `dxvk.conf` | shipped with no active setting: the defaults are already right for KosmicKrisp |
| Linux smoke test | **blocked**: Ubuntu's Wine 9.0 is too old for DXVK 3.1.1 (see [Validation](#validation-on-linux)) |

## Build

```bash
scripts/setup.sh                      # llvm-mingw into toolchains/
```

```bash
graphics/dxvk/build.sh                # arm64x + i386 (what a Mac bundle ships)
```

```bash
graphics/dxvk/build.sh x86_64 i386    # for Linux validation with x86_64 Wine
```

Needs git, meson, ninja, glslang (`glslang` or `glslangValidator`) and python3. The script:

1. clones DXVK, fetches the pinned tag and **fails unless it resolves to the pinned commit**,
   then checks out that commit with its submodules (Vulkan/SPIR-V headers, dxbc-spirv,
   libdisplay-info) at the recorded commits;
2. applies `graphics/dxvk/patches/*.patch` (none at present);
3. configures one meson tree per target with the cross files in
   [`graphics/dxvk/cross/`](../graphics/dxvk/cross), as a release build, stripped. Each tree is
   reconfigured from scratch when the commit, patches, cross file or clang version change;
4. builds only the five DLL targets and stages them in
   `graphics/dxvk/out/dxvk-<git describe>/<arch>-windows/`, with `VERSION`, `COMMIT`,
   `PATCHES` and a copy of `dxvk.conf`;
5. checks every staged DLL: the 64-bit ones must be ARM64X (PE machine `aa64` and
   `llvm-readobj --coff-load-config` format `COFF-ARM64X`) and export their entry point
   (`D3D11CreateDevice`, `Direct3DCreate9`, ...) **in both views**; the i386 ones must be
   machine `014c`. Setting `WINEBUILD=/path/to/winebuild` also marks the copies as Wine
   builtins (`winebuild --builtin`), which a bundle that installs them into `lib/wine` needs
   (see [mach9](#adding-dxvk-as-a-backend-in-mach9)).

A full three-target build takes about 6.5 minutes on 4 cores.

### How the ARM64X build works

DXVK ships no ARM64 cross file, but none is needed beyond the one in
`cross/llvm-mingw-arm64x.txt`. It copies the approach of vkd3d-proton's `build-arm64x.txt`,
which athei's `mach9/bin/build-vkd3d-proton.sh` uses, and of DXMT's `build-arm64ec.txt`: use
the `arm64ec-w64-mingw32-*` llvm-mingw tools and pass `-marm64x` to both compile and link.
Clang then compiles every translation unit twice (ARM64EC and ARM64) and lld links both halves
into one image. An x64 game, which runs as ARM64EC under FEX, gets the ARM64EC half, so DXVK
runs natively inside an emulated game. A native ARM64 program gets the ARM64 half. Unlike
vkd3d-proton's file, ours sets no `-march=armv8.2-a -mtune=cortex-x3`. Baseline armv8-a code
runs on every M-series chip, and the tuning is for a Qualcomm core.

DXVK's source already handles ARM64EC. `src/util/util_bit.h` defines `DXVK_ARCH_ARM64` (not
`DXVK_ARCH_X86`) for `_M_ARM64EC`, so the SSE intrinsics paths are compiled out, and the
`-msse*` / `-mfpmath=sse` flags are dropped by meson's `get_supported_arguments`. No source
change was needed.

What `llvm-readobj` reports for the staged 64-bit DLLs:

```
aarch64-windows/d3d10core.dll: Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E) | Format: COFF-ARM64X
aarch64-windows/d3d11.dll:     Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E) | Format: COFF-ARM64X
aarch64-windows/d3d8.dll:      Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E) | Format: COFF-ARM64X
aarch64-windows/d3d9.dll:      Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E) | Format: COFF-ARM64X
aarch64-windows/dxgi.dll:      Machine: IMAGE_FILE_MACHINE_ARM64X (0xA64E) | Format: COFF-ARM64X

$ llvm-readobj --coff-load-config aarch64-windows/d3d11.dll    # CHPE code map
  CodeMap [
    0x1000 - 0x3F0920  ARM64
    0x3F1000 - 0x82C2A0  ARM64EC
    0x82D000 - 0x82E050  X64          <- entry/exit thunks
  ]
$ llvm-readobj --coff-exports aarch64-windows/d3d11.dll | grep -c 'Name: D3D11CreateDevice$'
2                                     <- exported by both halves
```

The ARM64X DLLs are about 1.7 times the size of the x86_64 ones (d3d11: 13.9 MB vs 7.9 MB),
since they hold two copies of the code.

## Audit: DXVK v3.1.1 vs KosmicKrisp

Input: `results/m0-kosmickrisp-m2-vulkaninfo.txt` (Apple M2, KosmicKrisp, Mesa 27.0.0-devel
`git-4d597245b7`, Vulkan 1.4.363, `DRIVER_ID_MESA_KOSMICKRISP`).

### Hard requirements: all met

DXVK decides whether to use a device in `DxvkDeviceCapabilities::checkDeviceCompatibility()`
(`src/dxvk/dxvk_device_info.cpp`). It needs:

- Vulkan 1.3;
- a graphics+compute queue;
- every entry `getFeatureList()` marks as required;
- at least 256 bytes of push constants, unless VK_EXT_descriptor_heap is in use.

[`graphics/dxvk/check-features.py`](../graphics/dxvk/check-features.py) reads that list from
the DXVK source itself, so it follows the pin, and checks it against a vulkaninfo text dump:

```
$ graphics/dxvk/check-features.py graphics/dxvk/work/src results/m0-kosmickrisp-m2-vulkaninfo.txt
device: Apple M2, KosmicKrisp Mesa 27.0.0-devel (git-4d597245b7)
DXVK entries: 164 (53 required); device extensions parsed: 153
required, missing: 0
verdict: DXVK accepts this device
```

The 53 required entries include `geometryShader`, `dualSrcBlend`, `multiViewport`,
`multiDrawIndirect`, `shaderInt64`, `shaderInt16`, `textureCompressionBC`,
`bufferDeviceAddress`, `vulkanMemoryModel`, the descriptor-indexing set, `robustBufferAccess2`
and `nullDescriptor` (VK_EXT_robustness2), `depthClipEnable`, `maintenance5`, `maintenance6`,
VK_KHR_load_store_op_none and VK_KHR_swapchain. Every one is present. `maxPushConstantsSize`
is exactly 256. A negative test confirms the checker catches gaps: a copy of the dump with
`shaderInt64 = false`, VK_EXT_depth_clip_enable removed and 128 bytes of push constants is
rejected with all three named.

### The three features KosmicKrisp lacks

**`shaderFloat64`: optional; D3D11 doubles are reported as unsupported.**
`D3D11DeviceFeatures` sets `DoublePrecisionFloatShaderOps` and
`ExtendedDoublesShaderInstructions` only when both `shaderFloat64` and `shaderInt64` are
present (`src/d3d11/d3d11_features.cpp`). So `CheckFeatureSupport(D3D11_FEATURE_DOUBLES)`
returns FALSE, as on any GPU without doubles. A game that creates a shader using DXBC double
opcodes anyway (`dadd`, `dmul`, `dtof`, ...) gets `E_INVALIDARG` from `Create*Shader`, with the
log line `Shader uses fp64, but feature is not supported` (`src/d3d11/d3d11_device.cpp`).
Doubles are an optional D3D11 cap, so well-behaved games check it first. A game that doesn't
fails visibly instead of rendering garbage. Doubles are not part of any feature level, so this
does not lower the feature level. D3D9 has no doubles. Emulating fp64 is a KosmicKrisp
(driver) question, not a DXVK one.

**`VK_EXT_graphics_pipeline_library`: optional; DXVK compiles full pipelines at draw time.**
`DxvkDevice::canUseGraphicsPipelineLibrary()` is false, so DXVK neither pre-compiles shader
libraries when shaders are created nor fast-links them. Every new combination of shaders and
state is compiled as a complete pipeline when it is first drawn, on the submission thread.
That is DXVK's pre-2.0 behaviour, and it shows as **shader-compilation stutter the first time
a scene is seen**. Mitigations:

- the driver's own pipeline cache (KosmicKrisp/Mesa disk cache): later runs skip compilation;
- `dxvk.enableGraphicsPipelineLibrary` and `dxvk.trackPipelineLifetime` do nothing here;
- the real fix is GPL (or VK_EXT_shader_object) in KosmicKrisp. Worth tracking upstream.

Two smaller consequences. `VK_EXT_shader_module_identifier` is also missing; DXVK uses it only
for pipeline-lifetime tracking with GPL, mostly on 32-bit. And `extendedDynamicState3` is only
partly there (see below), so a few rasterizer and blend states that would be dynamic on
desktop drivers are baked into the pipeline, which adds pipeline variants.

**`VK_EXT_descriptor_buffer` (and `VK_EXT_descriptor_heap`): optional; DXVK uses descriptor sets.**
The `DxvkContext` constructor picks descriptor heap, then descriptor buffer, then a classic
`DxvkDescriptorPool` with `vkUpdateDescriptorSets` (`src/dxvk/dxvk_context.cpp`). KosmicKrisp
has neither extension. DXVK would not use them here anyway: `disableUnusedFeatures()` turns
both on only for allowlisted drivers (RADV, NVK, llvmpipe, ANV, AMD, newer NVIDIA), and
KosmicKrisp is not on the list. So DXVK takes the descriptor-set path. That path is fully
supported and is what DXVK used everywhere before 2.x. The cost is CPU overhead per draw
(descriptor-set allocation and updates), which on an M-series CPU matters less than
pipeline compilation. `dxvk.enableDescriptorUpdateTemplates` (default off) switches that path
to update templates; it is a candidate to measure with `gpu11.drawcalls` on a Mac, not a
default.

### Other optional gaps worth knowing

Run with `--all` for the full list (44 optional entries absent). The ones with a visible effect:

| Missing | Effect in DXVK |
|---|---|
| VK_EXT_conservative_rasterization, VK_EXT_fragment_shader_interlock | No conservative raster or ROVs, so **the D3D11 feature level tops out at 12_0**. `GetMaxFeatureLevel` also needs typed-UAV-load formats for 12_0; vulkaninfo was run without `--show-formats`, so whether we get 12_0 or 11_1 still needs a check on a Mac (mpbench's `gpu11.device` prints it). Sparse/tiled resources tier 2 needs `sparseBinding`, `sparseResidencyBuffer/Image2D/Aliased`, a sparse queue and `shaderResourceResidency/MinLod`, and KosmicKrisp has them all. |
| `depthBounds` | D3D11 has no depth-bounds test; D3D9's NVDB hack is not offered. Harmless. |
| VK_EXT_depth_bias_control | KosmicKrisp has no `D24_UNORM_S8_UINT` (Apple GPUs lack it; only D16/D32F/S8/D32FS8 appear in the dump), so DXGI remaps every D24 format to `D32_SFLOAT_S8_UINT` (`src/dxgi/dxgi_format.cpp`). Without depth_bias_control DXVK cannot ask for unorm-style depth bias on that float buffer, so **D3D depth bias (decals, shadow-map acne fixes) on D24 surfaces is scaled differently**. Expect occasional z-fighting or shadow acne differences. Fixing it means adding the extension in KosmicKrisp. |
| `wideLines`, VK_EXT_line_rasterization (rectangular/smooth) | D3D9 wide and antialiased lines fall back to 1-pixel lines. |
| EDS3 `AlphaToCoverageEnable`, `RasterizationSamples`, `SampleMask` | Those states become pipeline-baked (more pipeline variants; see GPL above). `DepthClipEnable`, `LineRasterizationMode` and `SampleLocationsEnable` are present. |
| VK_EXT_memory_priority, VK_EXT_pageable_device_local_memory | No residency hints; irrelevant on unified memory. |
| VK_KHR_maintenance11, VK_EXT_dynamic_rendering_unused_attachments, VK_EXT_border_color_swizzle `FromImage` | Minor; DXVK has fallbacks. |
| VK_KHR_external_memory_win32, VK_KHR_win32_keyed_mutex, VK_EXT_full_screen_exclusive, VK_KHR_incremental_present, NV/NVX extensions | Not driver features on macOS. winevulkan provides the win32 ones; the rest are NVIDIA- or Windows-only. |
| VK_EXT_swapchain_colorspace | Listed by the checker because DXVK tracks it with device features, but it is an **instance** extension, and KosmicKrisp has it. HDR is not affected. |

### What DXVK already does right for KosmicKrisp

- **Tiler mode is on automatically.** `DxvkDevice::getPerfHints()` lists
  `VK_DRIVER_ID_MESA_KOSMICKRISP` among the tile-based drivers, so DXVK keeps render passes
  intact and prefers cached host memory.
- **D3D9 float emulation** is `Enabled`, not `Strict`: KosmicKrisp is not on the list of
  drivers with a native mul-zero instruction. That is DXVK's normal setting for most drivers.
- **Unified memory.** With one DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED heap, DXVK sees a
  UMA device. It does not enforce a separate VRAM budget, and DXGI reports the heap (11.84 GiB
  in the dump) as both dedicated and shared memory.
- `LowerConstantArrays` is on, because KosmicKrisp is not on DXVK's list of Mesa drivers that
  optimise large constant arrays themselves. That is the conservative choice for a new driver.
  If KosmicKrisp turns out to handle them well, a one-line patch to add it to that list is a
  candidate, but only once a Mac measurement shows a gain.

### Caveat: Wine's winevulkan sits in between

The audit uses the driver's native feature list. A Windows DLL sees what **winevulkan** exposes,
and winevulkan hides any extension its generated tables don't know. That bit us on Linux:
Wine 9.0's winevulkan does not know VK_KHR_load_store_op_none (which DXVK 3.1.1 requires), so
DXVK 3.1.1 rejects lavapipe there although lavapipe has it. mach9 is on Wine 11.19/11.20 and
Proton on 11.0, both much newer, but confirm on the Mac. Run a Windows `vulkaninfo.exe` under
the bundle's Wine, save the text output, and feed it to `check-features.py`. That checks the
whole chain: KosmicKrisp, the Vulkan loader and winevulkan.

## `dxvk.conf`

[`graphics/dxvk/dxvk.conf`](../graphics/dxvk/dxvk.conf) ships with **no active setting**. The
audit found nothing that should change globally: the missing features are already handled
by DXVK's fallbacks, tiler mode is already on, and the options tied to GPL, descriptor buffers
and descriptor heaps do nothing on this driver. The file documents the candidates for per-game
tuning, each to be turned on only after a measurement:

| Option | Why it might help on KosmicKrisp |
|---|---|
| `dxvk.tilerMode = True` | Already automatic for KosmicKrisp's driver ID. Only matters if a future build reports another ID; set `False` to measure the difference. |
| `d3d11.maxTessFactor = 16` | Metal's tessellation model differs from Vulkan's, so KosmicKrisp translates it at a cost not yet measured. A cap trades detail for speed in tessellation-heavy games. |
| `dxgi.customVendorId = 1002` | DXGI reports vendor 0x106b (Apple). For the few games that refuse an unknown vendor, report AMD. |
| `dxgi.maxDeviceMemory = 6144` | The single unified heap is reported as ~12 GiB of VRAM; games that size streaming pools from it may oversubscribe RAM. |

## Patches

[`graphics/dxvk/patches/`](../graphics/dxvk/patches) is empty (it holds only a README with the
policy). Nothing in DXVK v3.1.1 needs changing for KosmicKrisp. Every hard requirement is met,
the missing optional features have fallbacks, and DXVK already knows KosmicKrisp's driver ID.
The work for the gaps above (fp64, GPL, depth_bias_control, D24) belongs in KosmicKrisp, not
in DXVK.

## Adding DXVK as a backend in mach9

athei's mach9 ([`bin/bundle-wine.sh`](https://github.com/athei/mach9/blob/main/bin/bundle-wine.sh)
Step 4, `bin/lib/profile.sh`, `docs/compatdb.md`; read at `d176766`) does not put Direct3D in
Wine's default DLL dirs. Each implementation gets its own tree, and a small `compatdb.so`
chooses one tree per *family* per process.

```
lib/wine/
  aarch64-windows/  i386-windows/     default dirs: only fake-module markers for every
                                      Direct3D DLL name any tree supplies (no real Direct3D)
  dxgi/<impl>/                        family "dxgi": D3D10/11 (+ D3D12) and dxgi.dll
      aarch64-windows/*.dll           64-bit, must be ARM64X
      i386-windows/*.dll
      aarch64-unix/*.so               only for implementations with a unix half (DXMT's winemetal)
  d3d9/<impl>/                        family "d3d9"
      aarch64-windows/ i386-windows/ [aarch64-unix/]
```

- The trees today: `DXGI_IMPLS="dxmt wined3d"` and `D3D9_IMPLS="mtld3d wined3d"` in
  `profile.sh`, in the order the markers prefer them. `dxgi/dxmt` holds DXMT's
  `dxgi/d3d10core/d3d11/winemetal`, Wine's `d3d10/d3d10_1`, and vkd3d-proton's
  `d3d12/d3d12core`, because a D3D12 swapchain goes through that tree's `dxgi.dll`.
  `dxgi/wined3d` and `d3d9/wined3d` hold Wine's own DLLs. `d3d9/mtld3d` holds mtld3d's
  `d3d9`/`mtld3d` plus a unix `mtld3d.so`. D3D8 and DDraw stay on wined3d, in the default dirs.
- **Selection:** `compatdb.so` prepends exactly one tree per family to the DLL path
  (`Rule::dxgi`, `Rule::d3d9`, or the arch default), so a process never mixes two DXGI stacks.
  The default dirs hold `winebuild --fake-module` markers whose placeholders in the prefix
  let the prepended tree's real DLL load.
- **64-bit markers are ARM64X** (`winebuild --fake-module -marm64x`), so x64 (ARM64EC) and
  native ARM64 processes get the same tree. That is why `check_trees_arm64x` requires
  **every 64-bit DLL of every tree to be ARM64X**. DXVK's `aarch64-windows/` output from
  `build.sh` is ARM64X, as required.
- Real DLLs in `lib/wine` must be **Wine builtins** (`winebuild --builtin`), as
  `build-vkd3d-proton.sh` does for vkd3d-proton. Run `build.sh` with `WINEBUILD` pointing at
  the Wine build tree's winebuild (64-bit) and the Wine SDK's (i386), or mark the copies at
  bundle time.

To add DXVK there:

1. **Stage DXVK** with `WINEBUILD=... graphics/dxvk/build.sh` (or a `bin/build-dxvk.sh` in
   mach9's style, with a `pins/dxvk-src.ref`), giving `aarch64-windows/` (ARM64X) and
   `i386-windows/`.
2. **`dxgi/dxvk` tree:** DXVK's `dxgi.dll d3d11.dll d3d10core.dll`, Wine's
   `d3d10.dll d3d10_1.dll` (as the dxmt tree takes them), and vkd3d-proton's
   `d3d12.dll d3d12core.dll`. DXVK's dxgi presents vkd3d-proton's swapchains; that is the
   standard Proton pairing. DXVK has no unix half, so no `aarch64-unix/`.
3. **`d3d9/dxvk` tree:** DXVK's `d3d9.dll`. For D3D8 through DXVK, also put DXVK's `d3d8.dll`
   here, since it calls into `d3d9.dll`. Then `family_dlls d3d9` includes `d3d8`, so Wine's
   default `d3d8.dll` becomes a marker, and `d3d9/wined3d` and `d3d9/mtld3d` each need their
   own copy of Wine's `d3d8.dll`.
4. **`profile.sh`:** add `dxvk` to `DXGI_IMPLS` and `D3D9_IMPLS`. Its position decides which
   real DLL each marker is stamped from, not which tree a process gets.
5. **`bundle-wine.sh` Step 4:** an `if in_list dxvk "$DXGI_IMPLS"` block that copies the
   files above, modelled on the dxmt block, and the same for `D3D9_IMPLS`.
   `check_trees_arm64x` and the per-tree smoke tests (`smoke: <arch> on <family>/<impl>`)
   then cover it automatically.
6. **compatdb:** tree names are an enum, not free text. Add `Dxvk` to `Dxgi` and `D3d9` in
   `compatdb/core/src/value.rs` (`ALL`, `as_str`), to `Key::values` in `key.rs`, and to the
   tests. Pick the default tree per arch in the `runtime` module's `default_dxgi` /
   `default_d3d9`. A per-game rule is then `dxgi = dxvk` / `d3d9 = dxvk`.
7. **Vulkan:** nothing to add. mach9 already bundles KosmicKrisp (`bin/build-mesa.sh` →
   `libvulkan_kosmickrisp.dylib` + `share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`)
   for vkd3d-proton, and DXVK reaches it through winevulkan in the same way.

mproton's own launcher (`launcher/mproton`, Proton style) needs no tree. It copies the backend's
DLLs into the prefix from `files/lib/wine/dxvk/<pe-arch>/`, with `aarch64-windows` for 64-bit
and `i386-windows` for WoW64, which is exactly `build.sh`'s output layout. These copies stay
native DLLs, so no builtin marking is needed.

## Validation on Linux

Goal: run mpbench's D3D11 scenes (`gpu11.*`) and, since
[jesscarter/mproton#3](https://github.com/jesscarter/mproton/pull/3) is merged, the D3D12 ones
(`gpu12.*`, DXVK's dxgi + vkd3d-proton 3.1), under Linux Wine with the x86_64 and i386 builds
of the same pin, on Mesa lavapipe. This is a correctness smoke test only.

```bash
graphics/dxvk/build.sh x86_64 i386
```

```bash
MPBENCH_ARCHS="x86_64 i686" sh bench/build.sh
```

```bash
scripts/validate-lavapipe.sh graphics/dxvk/out/dxvk-v3.1.1 path/to/vkd3d-proton-3.1 results/dxvk-v3.1.1-lavapipe
```

(`validate-lavapipe.sh` now also takes `build.sh`'s `x86_64-windows/` / `i386-windows/`
layout, makes the output dir absolute, and creates `MPBENCH_WORK`.)

**Result: not completed. The only Wine available here is too old for DXVK 3.1.1.** This session
runs Ubuntu 24.04, whose Wine is 9.0. Its winevulkan does not know
VK_KHR_load_store_op_none, which DXVK 3.1.1 requires. lavapipe itself exposes it
(Mesa 25.2.8), but Wine 9.0's winevulkan filters it out. Both builds load, initialise and
enumerate the device, then reject it
([`results/dxvk-v3.1.1-lavapipe/wine-9.0-smoke.txt`](../results/dxvk-v3.1.1-lavapipe/wine-9.0-smoke.txt)):

```
info:  DXVK: v3.1.1
info:  Build: x86_64 clang 23.1.5                 (and: x86 clang 23.1.5)
info:  Vulkan: Found vkGetInstanceProcAddr in winevulkan.dll
info:  Found device: llvmpipe (LLVM 20.1.2, 256 bits) (llvmpipe 25.2.8)
info:    Skipping: Device does not support required feature 'khrLoadStoreOpNone' (extension: VK_KHR_load_store_op_none)
warn:  DXVK: No adapters found.
gpu11.device  error  D3D11CreateDeviceAndSwapChain(...): 0x80004005
```

The D3D12 scenes fail the same way, at `CreateDXGIFactory1`, because vkd3d-proton gets its DXGI
from DXVK. So the run proves that the llvm-mingw x86_64 and i386 builds load under Wine, run
DXVK's instance setup and reach device selection. It does **not** yet prove rendering. The
earlier lavapipe results (`results/linux-lavapipe-*.json`, from PR #3) used the DXVK 2.7.1
release on the same Wine 9.0. That version did not require the extension, which is why those runs passed.

To finish it, rerun the commands above with a Wine new enough for DXVK 3.1.1: WineHQ's packages,
or Wine 11.0 (Proton's base) built from source. dl.winehq.org is blocked by this environment's
network policy, and building or running a third-party Wine was not allowed in this session
without the user's go-ahead.
