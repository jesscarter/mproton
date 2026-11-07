# mproton: Proton for macOS (Apple Silicon M1–M3), research notes and skeleton

Status: exploratory. Written 2026-11-06. Upstream Proton is in `ref/proton` (shallow clone, no submodules, HEAD `bc89621`, Oct 2026).

## TL;DR

- **The timing is better than it was.** Three things that used to block this have now landed:
  1. Proton now builds for ARM64. To support ARM hardware, upstream Proton already builds `aarch64-unix` + `aarch64-windows` + `arm64ec-windows` with **FEX** for x86 emulation (`Makefile.in:80`, `proton:574`, `proton:2075`, `toolmanifest_arm64.vdf`).
  2. macOS 26.4/26.5 added the OS pieces ARM64 Wine needs:
     - 4K-page processes via `posix_spawnattr_set_4k_page_size_np`
     - per-thread hardware TSO via `thread_set_x86_64_compat`
     - an x18 ABI mode
     - `-Wl,-x86_64_layout_emulation` for a low-4GB PAGEZERO

     Apps unlock these with the `com.apple.developer.cross-architecture-support` entitlement. CodeWeavers already ships ARM64 Wine + FEX on macOS 26.5 in CrossOver Preview.
  3. **KosmicKrisp** (LunarG's Mesa driver, Vulkan on Metal) is Vulkan 1.4 conformant on Apple Silicon. Khronos now recommends it over MoltenVK. With community patches it gains geometry shaders, transform feedback, sparse resources and 64-bit atomics, which DXVK and vkd3d-proton need.
- **Rosetta 2 has an end date.** General Rosetta support ends with macOS 27, and Intel-only apps stop launching in macOS 28. Only a vague exception for "older unmaintained games" remains. A new project should **not** build on x86_64 Wine under Rosetta.
- **Recommended shape:** "Proton's existing ARM64 configuration, retargeted from `aarch64-linux` to `aarch64-darwin`." That means arm64 Wine (Proton's fork, rebased with macOS ARM64 patches), FEX through ARM64EC/WoW64, DXVK + vkd3d-proton on KosmicKrisp, and DXMT as an optional fast path for D3D11.
- **This laptop runs macOS 14.0.** The ARM64 path needs **macOS 26.5+**, so upgrade before doing serious work. M1, M2 and M3 can all run it.

## Prior art (what to learn from or borrow)

| Project | Approach | Relevance |
|---|---|---|
| **CrossOver** (CodeWeavers, commercial; Wine source is LGPL and published) | x86_64 Wine under Rosetta. The ARM64 + FEX Preview (Jul 2026+) needs macOS 26.5. Graphics: wined3d, DXVK/MoltenVK, D3DMetal, DXMT | The reference implementation. Their Wine source drops are the best macOS ARM64 Wine patches until they land upstream (Brendan Shanks said upstream MRs are coming "soon", Aug 2026). |
| **Whisky** | SwiftUI front end over CrossOver Wine + GPTK | Discontinued in 2025. Useful UX reference only. |
| **Game Porting Toolkit / D3DMetal** (Apple) | Closed D3D11/12 → Metal | Best D3D12 performance on Mac today. It is proprietary and its redistribution terms are **unverified**, so it can only be a "bring your own" option. |
| **DXMT** (3Shain, LGPL-2.1) | D3D10/11 directly on Metal, using a `winemetal` unixlib | Fast D3D11. Needs small Wine patches (it uses `winemac.so` symbols). Has ARM64 builds through CrossOver. |
| **MetalSharp-Wine** | Wine 11.20 native arm64 + FEX fork. Graphics: Mullion (DXMT fork), DXVK-macOS, vkd3d-proton-macOS, MoltenVK | The closest open-source analogue to this plan. Read its signing and entitlement setup. |
| **athei/mach9 (wine-build)** | arm64 Wine 11.19 + 17 patches. The loader re-execs itself for 4K pages; hardware TSO is on by default (`FEX_HARDWARE_TSO=0`) | A small, readable patch stack for the 4K-page and TSO work. |
| **steamac** (fxgl, Apache-2.0) | Runs the SteamOS ARM64 image **in a libkrun VM** (Hypervisor.framework). Graphics go Venus → KosmicKrisp/MoltenVK. It carries ~90 KosmicKrisp patches | A different architecture: it sidesteps the macOS host problems entirely. **Its KosmicKrisp patches are directly reusable.** |
| **NotProton** | Forces the macOS Steam client's dormant Steam Play feature on and uses a patched CrossOver as the "compat tool" | Shows the Steam-client integration works. Copy how it registers the tool and selects the depot. |
| **Kaon** (natbro) | Sets `steam_dev.cfg` to force the Windows platform type in macOS Steam | An older, hackier version of the same Steam-side idea. |

## Architecture skeleton

```
macOS Steam client (native arm64)  ──Steam Play (forced on)──▶  mproton compat tool
                                                                 │
  mproton launcher (Python, fork of `proton`)                    │
   - prefix management, dll placement, per-game config           │
   - drop pressure-vessel/Steam Linux Runtime; add mac paths     │
                                                                 ▼
  wine (arm64 macOS host, Proton fork + macOS ARM64 patches)
   ├─ unix side:  aarch64-darwin .so/.dylib (ntdll.so, winemac.drv, winecoreaudio, IOHID winebus, msync)
   ├─ PE side:    aarch64-windows + arm64ec-windows builtins
   ├─ x86 code:   FEX (libarm64ecfex.dll for x86_64, libwow64fex.dll for i386)
   │              + 4K-page spawn + per-thread TSO
   ├─ lsteamclient ─▶ steamclient.dylib   (an __APPLE__ path already exists: lsteamclient/unixlib.cpp:750)
   └─ graphics:
        D3D9/10/11 ─▶ DXVK ─┐                 (or ─▶ DXMT ─▶ Metal, optional)
        D3D12      ─▶ vkd3d-proton ─┤
        Vulkan     ─────────────────┼─▶ winevulkan ─▶ KosmicKrisp (Mesa) ─▶ Metal
        OpenGL     ─▶ wined3d/opengl32 ─▶ Zink? / Apple GL 4.1 (legacy)
        (D3DMetal: bring-your-own, not shipped)
```

DXVK on a Vulkan driver keeps Proton's graphics layer **identical to upstream**. That preserves the value of Proton's per-game workarounds, and DXVK/vkd3d-proton bug fixes flow through for free. All driver-gap work then lands in one place: KosmicKrisp, the Vulkan-on-Metal driver. DXMT is a second backend for D3D11, chosen per game for speed.

## Workstreams

### 1. CPU / binary format (hardest OS-level work)
- **Target:** arm64 Wine host, with x86 guest code translated by FEX. Use ARM64EC for x86_64 and WoW64 for i386. Don't plan around Rosetta.
- **Needed:**
  - 4K pages for Windows processes (`posix_spawnattr_set_4k_page_size_np` plus a loader re-exec)
  - hardware TSO per thread (`thread_set_x86_64_compat`, which **new threads don't inherit**, so it must be applied in Wine's thread-creation path)
  - an x18-safe ABI
  - low-memory reservation through `-x86_64_layout_emulation`
- **Packaging:** the entitlement requires a signed `.app` with an embedded provisioning profile, for both the outer app and the inner `wine` loader. A paid Apple Developer account gets the `-support` variant; free accounts get `-unmanaged`. Disabling SIP also works, but only for local hacking.
- **FEX:** upstream FEX has no macOS port. CodeWeavers and MetalSharp each carry a fork, so check both for patches to rebase.
- **Fallback for older macOS:** x86_64 Wine under Rosetta, the classic CrossOver build. Treat it as a bootstrap or debug path only.

### 2. Wine
- Proton's Wine fork is Linux-first: fsync/ntsync, winex11/winewayland, Steam Deck hacks. Two ways to get a macOS-capable tree:
  - **(A, recommended)** Proton Wine + the macOS ARM64 patch stack (from CodeWeavers' source drop, MetalSharp, or athei), resolving conflicts. This keeps Proton's game fixes.
  - **(B)** CrossOver Wine + selected Proton patches. Easier to bring up on macOS, but it drifts from Proton.
- **Synchronization:** ntsync and fsync are Linux-only. Use msync (Mach semaphores; `WINEMSYNC` in CrossOver-derived builds).
- **Drivers:** Wine already has macOS drivers for window/input/audio/HID: `winemac.drv`, `winecoreaudio.drv`, and winebus on IOHID. Check how well Proton's patches to winex11 and winebus carry over (fullscreen, raw mouse, Steam Input).

### 3. Graphics
- **KosmicKrisp:** build it from Mesa with steamac's patch series and track upstream. Gaps to watch:
  - geometry shaders and transform feedback (emulated)
  - tessellation (Metal's model differs)
  - sparse residency (needs Metal 4 placement heaps)
  - 64-bit atomics
  - descriptor-indexing limits
  - DXR on M1/M2 (no ray-tracing hardware; M3 has it)
- **DXVK / vkd3d-proton:** these are pure PE DLLs, so build them for `arm64ec` the way Proton's ARM64 build does. Expect mainly driver-feature-negotiation fixes, not Metal work.
- **DXMT:** optional per-game backend. Build `winemetal` against our Wine.
- **D3DMetal:** support detecting a user-installed GPTK and opting into it. Don't redistribute it.
- **winevulkan:** on macOS it calls into the MoltenVK/KosmicKrisp ICD with surfaces created through `winemac.drv` (Metal layer). Check how it handles `VK_EXT_metal_surface`.

### 4. Steam integration
- Register as a compat tool in the macOS client. The Steam Play code is present but inert, and NotProton and Kaon show it can be forced on. Find out exactly how (config key? `compatibilitytools.d` path on mac?) and whether `toolmanifest.vdf` needs changes. Drop `require_tool_appid` (that's the Steam Linux Runtime).
- **Depots:** the Windows depot is selected when a compat tool is forced. Fallback: `steamcmd +@sSteamCmdForcePlatformType windows` (unverified on macOS).
- **lsteamclient:** already has an `__APPLE__` branch that loads `$STEAM_COMPAT_CLIENT_INSTALL_PATH/steamclient.dylib`. That needs the arm64 macOS Steam client, which is now native. Port `steam_helper` (`steam.exe`) the same way.
- **Overlay, Steam Input, cloud saves:** check each one. The overlay is likely broken at first.

### 5. Launcher (`proton` script → `mproton`)
- 2,189 lines of Python. Linux-specific parts:
  - pressure-vessel and runtime paths
  - `LD_LIBRARY_PATH` (→ `DYLD_*`, but SIP strips those vars; prefer rpath)
  - `/proc` usage
  - fsync/ntsync flags
  - the FEX config generator (`proton:1684`), which mostly carries over
- Keep the dll-placement and per-game compat-config tables. They are where Proton's accumulated knowledge lives.

### 6. Build system
- Proton builds inside a Linux Docker/Podman container (the Steam Runtime SDK). macOS needs a native host build:
  - Xcode clang for `aarch64-darwin` unix modules
  - llvm-mingw for `aarch64-windows`/`arm64ec-windows` PE (it supports ARM64EC)
- Rewrite the `rules-*` host targets in `Makefile.in` (or replace them with a simpler orchestrator).
- **Gstreamer/ffmpeg:** these work on macOS; use Homebrew or vendored builds.
- **Drop:** OpenVR/OpenXR, piper/kaldi/vosk (the accessibility TTS/STT bits; optional later), pressure-vessel.

### 7. Explicitly out of scope (at least initially)
- **Kernel/anti-cheat games:** EAC and BattlEye have Linux-native modules from the vendors and no macOS equivalent. Treat them as unsupported.
- **32-bit-only DRM/launchers:** these need i386 through WoW64 + FEX, which costs more. Defer.
- **Intel Macs.**

## Milestones (each one demoable)

0. **Baseline.** Upgrade to macOS 26.5+. Run an existing ARM64 Wine (CrossOver Preview or MetalSharp) and a D3D11 game by hand. Record FPS and correctness to beat.
1. **Toolchain.** Native build of upstream Wine arm64 + llvm-mingw ARM64EC. `wine notepad.exe` (arm64) runs.
2. **x86.** FEX ARM64EC builds and loads. An x86_64 console `.exe` runs with 4K pages and TSO, verified with a page-granularity test and a memory-ordering litmus test.
3. **Proton Wine.** Proton's Wine fork + macOS patches builds. Wine conformance tests pass at roughly the same rate as stock.
4. **Graphics.** KosmicKrisp + DXVK renders a D3D11 sample, then vkd3d-proton renders a D3D12 sample. DXMT is an alternative.
5. **Launcher.** `mproton run game.exe` works from the CLI with prefix creation and dll placement.
6. **Steam.** macOS Steam launches a Windows-only game through mproton. Achievements work, which confirms lsteamclient.
7. **Compatibility harness.** Scripted install → launch → screenshot → frame-time capture across a fixed game list. This is the regression gate for every Wine, FEX or KosmicKrisp bump.

## Open questions to settle early
- **Apple entitlements:** can an OSS project obtain and use `cross-architecture-support` for distribution, or must each user build and sign their own copy?
- **API versions:** sources disagree on whether the 4K-spawn API is 26.0 or 26.5. Check the SDK headers directly.
- **Steam's switch:** the exact mechanism that turns on dormant Steam Play in macOS Steam, and how stable it is across client updates.
- **D3DMetal license:** whether redistribution is allowed at all.
- **TSO cost:** performance of FEX with hardware TSO vs FEX's software TSO on M1–M3.
- **FEX upstream:** whether it will accept macOS support, or whether we carry a fork.
