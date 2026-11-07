# Proton Wine × macOS ARM64: patch triage

Status: analysis done on Linux (2026-11-07). No Mac was used. Read "What this could not verify" before trusting any macOS claim.

This is the input for milestone 3 in [macos-port-plan.md](macos-port-plan.md) ("Proton's Wine fork + macOS patches builds"). It covers option A: keep Proton's Wine fork and rebase a macOS ARM64 patch stack onto it.

## TL;DR

- **The real obstacle is the base version, not Proton.** Proton's Wine (every branch, including `bleeding-edge`) is wine-11.0 plus 1454 commits. Both open macOS ARM64 stacks (athei, MetalSharp) sit on **wine-11.20**, which is 5054 upstream commits newer. Most of their conflicts happen on plain wine-11.0 as well:

  | stack | onto wine-11.20 | onto WineHQ master (11.21+) | onto vanilla wine-11.0 | onto Proton 11 (`proton_11.0`) |
  |---|---|---|---|---|
  | athei `arm64-patched` (36 commits) | 36 | 27 | 14 | 12 → **16** after 4 hand merges |
  | MetalSharp `source/wine` (131 commits, cumulative) | 131 | 108 | 65 | 47 |

  Only 4 of the 36 athei patches conflict *because of* Proton, and each was a small hand merge (below). The other 20 that fail on Proton also fail on vanilla 11.0, because they depend on 11.x code: the virtual-memory rewrite, win32u client surfaces and EGL, and inproc-sync API changes.
- **Recommendation:** don't backport the macOS stack to 11.0. Carry it on WineHQ master now, and do the real Proton merge on **Proton 12** (wine-12.0 base; wine-12.0 is due around January 2027, and recent Proton majors have followed each wine X.0 by a few months). Roughly 275 of Proton's 1454 commits are already upstream, so about 1180 Proton commits will need to come across. Until then, use [CrossOver 26](#crossover) (also wine-11.0) as the source for msync and winemac fixes if we need a working Proton 11 tree sooner.
- **Proton's Wine has darwin build blockers of its own**, found by reading the code: an unguarded `<linux/types.h>` via `ntsync_tmp.h`, unguarded futex code in `fsync.c`, glibc `mallopt`/`<malloc.h>` in `ntdll/unix/loader.c`, and a hard libdrm dependency in `amd_ags_x64`. The libdrm one breaks this Linux build too. All of these are small `#ifdef`/configure fixes. They're listed [below](#darwin-build-blockers-in-protons-tree).
- Proton's 1454 commits split roughly into **853 portable, 369 likely-conflict, 181 needs-a-macOS-equivalent, 51 Linux-only** (heuristic classification; per-commit list in `wine-patches/triage/proton-commits.tsv`).

## Inputs and pins

| what | ref | notes |
|---|---|---|
| Proton | `ValveSoftware/Proton` `proton_11.0` @ `bc896217` | default branch; `wine` submodule → `e60892cc` |
| Proton Wine | `ValveSoftware/wine` `proton_11.0` @ `e60892cc239` | `git describe`: `wine-11.0-1454-ge60892cc239`. `experimental_11.0` and `bleeding-edge` are also wine-11.0 based |
| upstream base | `wine-11.0` (`db11d0fe6a1`, 2026-01-13) | from the `wine-mirror/wine` GitHub mirror: gitlab.winehq.org is blocked from this sandbox |
| athei | `athei/wine` `arm64-patched` @ `5959a04b1af` (wine-11.20 + 36) | the ref `athei/mach9` pins in `pins/wine-src.ref`; FEX is `athei/FEX` `macos-patched` |
| MetalSharp | `metalsharp/MetalSharp-Wine` `source/wine` @ `1c97585b3db` (wine-11.20 + 131) | same commits as `patches/wine/0001..0131` on `main` |
| WineHQ master | `78e52205d9b` (wine-11.21-164, 2026-11-06) | stands in for the wine-12.0 base of a future Proton 12 |
| CrossOver | not fetched from CodeWeavers (media.codeweavers.com is blocked here) | see [CrossOver](#crossover) |

## 1. Proton's commits on top of wine-11.0

`git log wine-11.0..proton_11.0`: **1454 commits, no merges**. 259 are marked `cherry picked from`, and 258 of those are in WineHQ master. By patch-id, 275 are already upstream. 372 have `HACK` in the subject and 20 are reverts. The top authors are Paul Gofman (555) and Rémi Bernon (298), then Conor McCarthy, Connor McAdams, Brendan McGrath and Arkadiusz Hiler.

Grouping was done by `wine-patches/scripts/classify-proton.py`. It matches subject and touched-file rules (first match wins) and has a few manual overrides. The groups are heuristic: use them to plan, not as a final verdict. "unix" counts commits that touch unix-side code (`*/unix/*`, unixlibs, server, loader, `*.drv`).

### Patch groups → category → action

| group | commits | HACK | unix | category | action on macOS |
|---|---:|---:|---:|---|---|
| multimedia (winegstreamer, mf*, media-converter, quartz, wm*) | 318 | 71 | 8 | **portable** | Keep. Needs a bundled GStreamer on macOS; athei and MetalSharp both ship one. Proton's media-converter plugin lives in the Proton repo, not Wine. |
| other PE-side game/app fixes (kernelbase, msvcrt, mscoree, msi, shell32, …) | 192 | 54 | 14 | **portable** | Keep as is. |
| GPU vendor libs (amd_ags_x64, atiadlxx, amdxc64, nvapi bits) | 112 | 0 | 9 | **portable** (PE) | Keep the PE side. `amd_ags_x64`'s unixlib needs libdrm with no configure guard (a build blocker). Make it optional, or disable the unixlib on macOS. `atiadlxx` reads `/sys/class/drm` at runtime and fails gracefully. |
| prefix / registry defaults (wine.inf, wineboot, fonts) | 67 | 8 | 48 | **portable** | Keep. Font substitutions overlap with athei's (Menlo for Lucida Console). |
| d3dx / d3dcompiler / ddraw / dxgi PE side | 52 | 8 | 0 | **portable** | Keep. |
| speech / TTS (windows.media.speech + Vosk, protontts + Piper) | 38 | 3 | 10 | **portable** | Keep. The unix libs are built only when libvosk/libpiper are found. `protontts/unixlib.c` includes `<linux/limits.h>` (fix to `<limits.h>` before enabling it on macOS). |
| Proton integration (steam.exe hooks, `SteamGameId` hacks, `PROTON_*`, `WINESTEAM*`) | 28 | 21 | 13 | **portable** | Keep. The `LD_PRELOAD` / `steamoverlay.so` parts are under "Linux-only". |
| networking (ws2_32, nsiproxy, iphlpapi, winhttp, wininet) | 27 | 2 | 1 | **portable** | Keep. The ICMP error-queue code is guarded by `HAVE_LINUX_ERRQUEUE_H`. |
| anti-cheat / launchers (EAC, BattlEye, EA, Ubisoft, CEF) | 19 | 7 | 1 | **portable** | Keep. EAC and BattlEye load `.so` runtimes; Mac runtimes are a separate question. Overlaps with MetalSharp 0020/0056 (launcher policy). |
| ntdll unix side (virtual memory, signals, sync, process, loader) | 128 | 53 | 91 | **likely conflict** | Hardest group. Touches the same files as the macOS core (`virtual.c`, `signal_arm64.c`, `loader.c`, `sync.c`, `thread.c`). Watch `WINE_SIMULATE_WRITECOPY`, the address-space limits (`PROTON_LIMIT_ADDRESS_SPACE`, 2G limit, release-reserved-memory) and `hacks_init()`, which sits where athei loads `compatdb.so`. |
| win32u (windows, messages, display, d3dkmt) | 59 | 13 | 0 | **likely conflict** | Driver-independent, so in principle portable. It conflicts with athei/MetalSharp win32u fixes (dce.c painting, sysparams display modes, d3dkmt caps). |
| OpenGL (opengl32, win32u opengl, wined3d) | 53 | 17 | 1 | **likely conflict** | Conflicts with athei's EGL-on-winemac and the wined3d Vulkan fixes. `opengl32/unix_wgl.c` reads `/proc/self/cmdline` (runtime-only, fails gracefully). |
| wineserver core | 41 | 2 | 41 | **likely conflict** | Registry, file security xattrs (has both Linux and Apple `fgetxattr` signatures, fine), timezones, foreground tracking. Conflicts with msync's `protocol.def`, `thread.c` and `process.c` changes. |
| ARM64 / ARM64EC / FEX / WoW64 | 32 | 3 | 16 | **likely conflict, but wanted** | This is Proton's own ARM64 work, and it's exactly what we need: FEX dll names, cooperative ARM64EC suspend, wow64 suspend helpers, `BTCpuNotifyProcessExecuteFlagsChange`, "x86 mono on aarch64", the FEX AVX ARM64EC context. Keep it all. It touches `signal_arm64*.c` alongside athei, and we saw one conflict (APC dispatcher). |
| winevulkan / win32u Vulkan / OpenXR / vrclient | 27 | 2 | 5 | **likely conflict** | Proton drops the generated thunks from git and regenerates them, which conflicts with athei 713c1db (thunk edit). Win32u `vulkan.c` overlaps the portability-enumeration patch (athei 2245b85, MetalSharp 0014). OpenXR/vrclient: drop on macOS. |
| fullscreen hack (fshack, win32u/opengl32 side) | 21 | 1 | 0 | **likely conflict** | In Proton 11 the fshack is win32u/opengl32 framebuffer scaling, not winex11, so it may simply work on winemac. But winemac has its own retina scaling and display-mode emulation (athei/CrossOver/MetalSharp letterboxing, MetalFX scaling), so expect double scaling. Gate it off on macOS first, then evaluate it. |
| loader / preloader / build system (configure, makedep, winebuild, tools) | 14 | 1 | 4 | **likely conflict** | Proton deletes the generated `configure` and `config.h.in` and has its own makedep changes (external sources, C++, different 64-bit names). Conflicts with the athei/MetalSharp configure and makedep patches (all resolved trivially here). |
| winex11.drv (window management, input, XInput2, Vulkan surfaces) | 84 | 24 | 78 | **needs a macOS equivalent** | Drop the code. Re-create the *behaviours* in winemac.drv: raw mouse input (→ GameController mouse, athei 5a1a2fc / MetalSharp 0038/0065/0120), cursor clip and grab in fullscreen (→ MetalSharp 0127), fullscreen and mode handling (→ native-fullscreen management, MetalSharp 0039/0101/0113; athei "hide Dock and menu bar"), focus and `WM_TAKE_FOCUS` (→ athei/CrossOver focus fixes, MetalSharp 0089). |
| winebus / HID / Steam Input / controllers | 71 | 21 | 4 | **needs a macOS equivalent** (partly portable) | PE side (dinput, xinput, hidclass, windows.gaming.input): keep. `bus_sdl.c`: portable if SDL is bundled (athei bundles SDL3 + sdl2-compat). hidraw/udev: Linux-only, use the IOHID bus instead. Steam Input virtual-controller matching needs a check against the macOS Steam client. |
| fsync / ntsync / in-process sync | 15 | 7 | 15 | **needs a macOS equivalent** | Replace with **msync**. Proton's fsync is a futex inproc-sync backend next to upstream ntsync; msync is a third backend in the same framework. Port the per-game fsync tuning knobs (`WINE_FSYNC_SIMULATE_SCHED_QUANTUM`, `YIELD_TO_WAITERS`, `HELP_SIMULATED_PULSE`, set for GTA V, FFXIII, God Eater, …) to msync or to the generic sync path. |
| audio drivers (winepulse, winealsa) & mmdevapi | 11 | 0 | 3 | **needs a macOS equivalent** | winepulse changes → check whether winecoreaudio needs the same behaviour (latency, device naming). mmdevapi changes are portable. |
| gamescope / Steam Deck | 22 | 15 | 13 | **Linux-only: drop** | They're all keyed on gamescope/steamcompmgr being present, so they're inert on macOS. Drop them when rebasing so they don't make conflicts. |
| Linux tracing / debug / runtime (ftrace, gpuvis, uffd, memfd, `LD_PRELOAD`, `/run/host`, wine64-preloader, SVE, Asahi TSO, seccomp syscall emulation) | 16 (+4 under ARM64) | 6 | 15 | **Linux-only: drop or `#ifdef`** | Most are already guarded (`USE_UFFD_WRITEWATCH`, `HAVE_MEMFD_CREATE`, `__linux__`). The unguarded ones are under [darwin build blockers](#darwin-build-blockers-in-protons-tree). The SVE xstate and Asahi TSO commits are Linux-only (M1–M3 have no SVE, and macOS TSO goes through `thread_set_x86_64_compat` in FEX). |
| winewayland.drv | 7 | 4 | 5 | **Linux-only: drop** | — |

"Pressure-vessel": the only Wine-side trace is `HACK: loader: Map /run/host to / in the link map`. The rest lives in the Proton repo's scripts, so drop that one commit.

## 2. The macOS ARM64 patch stacks

### athei (`athei/wine` `arm64-patched`, used by mach9)

36 commits on wine-11.20. Several are squashes of many logical changes, and the commit messages are unusually thorough. Credits: Hadron (low reserve, W^X, ID registers), CrossOver (msync, `macdrv_functions`, compatdb hooks).

| # | commit | what it does |
|---|---|---|
| 1 | `39d1ebc` configure, loader, ntdll: arm64 macOS address space and app bundles | Keeps the default 4 GB `__PAGEZERO` (arm64 macOS SIGKILLs smaller ones). With the `com.apple.developer.cross-architecture-support` entitlement the kernel leaves one hard page and maps the rest as replaceable PROT_NONE, which the loader claims as Wine's low reserve. **The loader re-execs itself via `posix_spawn` + `posix_spawnattr_set_4k_page_size_np` to get 4K pages** before mapping anything. It refuses to run if the low 4 GB can't be mapped (no entitlement), and re-execs for a new slide when the 4 GB–loader gap is under 32 MB (for non-relocatable exes at 0x100000000). ntdll spawns child Wine processes directly with 4K pages (`POSIX_SPAWN_SETEXEC`). Finds `ntdll.so` from `<name>.app/Contents/MacOS`. |
| 2 | `f0cc31b` makedep | Links resources of ARM64EC-only modules (`vcruntime140_1` version resource). |
| 3 | `7d58e47` ntdll: reserved-area alignment | Fixes an 11.20 regression in `map_reserved_area()` hit by FEX's 256 MB-aligned allocations. |
| 4 | `e8e8cc4` ntdll: run x86 code through FEX on arm64 macOS | W^X page flipping (RW⇄RX on fault) and concurrent flips. **x18: opts every thread into the custom x18 ABI (macOS 26.4+, needs `com.apple.security.custom-x18-abi-toggle`)** so the TEB in x18 survives context switches. Keeps the top 16 MB free (XNU `copyinstr`). `MEM_EXTENDED_PARAMETER_WINE_UNIX_JIT` gives the FEX JIT `MAP_JIT` memory with `pthread_jit_write_protect_np` instead of flipping. Synthesizes the ARM64 ID registers from `hw.optional.arm.*` so FEX exposes SSE4.2/AES/SHA. Skips host mappings in free-space search. Alert hand-off between FEX-internal waits and guest waits. Clean crash on signals on threads without a TEB (the Cocoa main thread). |
| 5 | `5638e0a` ntdll: load `compatdb.so` before first PE code | Port of CrossOver CW Hack 24067: per-app rules rewrite the env and DLL path and load order before FEX initializes. |
| 6 | `2245b85` ntdll, win32u: self-contained bundle | Spawns via `wine.app/Contents/MacOS/wine` so children keep the bundle identity and entitlements. compatdb hooks for DPI, large-address-aware, GCMouse, NX and OpenGL choice. `VK_DRIVER_FILES` and GStreamer plugin defaults. Vulkan portability enumeration. ARM64X fake modules. |
| 7 | `f1fe4a2` ntdll: compatdb can force NX | Avoids FEX treating all writable memory as code. |
| 8 | `acb1029` winemac.drv: export `macdrv_functions` | CrossOver-compatible table for DXMT/mtld3d (CW HACK 22435), with client-surface lifetime fixes. |
| 9 | `5a1a2fc` winemac.drv: window, input and OpenGL-on-EGL fixes | ~26 fixes: cursor handling, Dock/menu-bar hiding instead of raising fullscreen windows, letterboxing, retina HORZRES, focus reporting, display modes without the safe flag, GameController raw mouse, IME placement, **OpenGL through EGL on Zink/KosmicKrisp**. |
| 10 | `17c1044` win32u: handle race, glyph metrics, painting | Includes "Disable OpenGL when the EGL display does not initialize". |
| 11 | `713c1db` assorted game fixes | Debugger handles, font aliases, PS4/PS5 Bluetooth rumble, `WINENCPU`, winevulkan struct conversion, WMA decoder DMO. |
| 12 | `324cd7b` winecoreaudio/winemac | Microphone permission prompt. |
| 13 | `7134955` server, ntdll: **msync** | CrossOver 26's msync (Marc-Aurel Zent; descends from esync/fsync), adapted to 11.20's inproc-sync API. Shared-memory objects, `__ulock_wait2`, a Mach message pump in wineserver for wait-multiple. **On by default** (`WINEMSYNC=0` turns it off); only wineserver decides. |
| 14–17 | wined3d / vkd3d-shader | Table fog, Apple GL buffer texture, Vulkan renderer spec violations and fallbacks (P8, windowless swapchain, X8D24), vs_1_x inputs. |
| 18, 23, 25 | tests | Skips for emulated ARM64 and timing. |
| 19–20, 26 | winemac.drv | Child client-view z-order over DXMT/mtld3d views; startup use-after-free. |
| 21 | `8a96f5d` server/mach.c | `STATUS_PARTIAL_COPY` on writes to read-only memory. |
| 22, 24, 32 | ntdll ARM64EC | `RtlIsEcCode` bounds, `QUEUE_USER_APC_CALLBACK_DATA_CONTEXT`, x64 breakpoint RIP. |
| 27–31, 33 | win32u OpenGL/EGL, display modes | EGL device and sRGB handling, modes below 60 Hz. |
| 34–36 | server/sock.c, wininet, dnsapi | kqueue connect failure, non-blocking connect, `.local` resolution at load. |

**TSO isn't in Wine at all in athei's setup.** `athei/FEX` (`macos-patched`, commit `ac6087ef` "Switch hardware TSO per thread on macOS") calls `thread_set_x86_64_compat(1)` directly with `svc #0x80` when a thread enters x86 code, and `(0)` at syscalls and unix calls (`Source/Windows/ARM64EC/Module.S`, `UnixLib/FEXUnixLib.cpp`). Because it's switched at every JIT entry, new threads need no hook in Wine's thread creation. That addresses the plan's worry that "new threads don't inherit it". It's on by default (`FEX_HARDWARE_TSO=0` turns it off) and gated by the same entitlement.

### MetalSharp (`metalsharp/MetalSharp-Wine`)

131 patches on wine-11.20, kept as `patches/wine/*.patch` on `main` and as commits on `source/wine`. A lot of the game and launcher work comes from WineForge (Radim Vesely); 0088–0100 are CrossOver 26.3 macOS fixes (see `docs/PROVENANCE.md` there). By theme:

- **Platform core (0001–0019, 0076, 0117–0118, 0126, 0128).** configure for native aarch64 macOS: 4 GB PAGEZERO and **x18 kept by linking the loader with `-platform_version macos <min> 12.0`**. The kernel only preserves x18 for main executables built against SDK ≤ 12, which is a different approach from athei's entitlement toggle. Without the entitlement it runs with 16K pages and no low 4 GB (0003): KUSER_SHARED_DATA is relocated and faulting loads from 0x7ffe0000 are emulated (0008). With the entitlement it releases PAGEZERO below 4 GB (0018) and defaults to 4K pages (0011, 0019: `posix_spawnattr_set_4k_page_size_np` on the loader exec, `WINE_4K_PAGES=0/1` overrides). There's also a W^X flip and emulation of stores into a W+X page from code in it (0006/0007/0010/0013/0015), ID registers (0004), FEX as the default emulator, and wow64/emulator notifications for cross-process writes and data views (0041, 0128).
- **msync (0035–0037, 0046–0047, 0070, 0078, 0116).** WineForge "WFUSync" (`os_sync_wait_on_address`/`__ulock`). It's a different code lineage from athei's CrossOver msync, but uses the same `WINEMSYNC` switch; on by default since 0078. 0116 drops the fd per in-process sync object.
- **FEX defaults (0063, 0066–0068, 0073, 0078).** TSO emulation was **off** by default (0063), then **on** (0078: RDR2 lost work without x86 ordering). This is *software* TSO; MetalSharp doesn't use `thread_set_x86_64_compat`.
- **winemac.drv (0012, 0029–0031, 0038–0039, 0057, 0065, 0088–0101, 0111, 0113, 0120, 0127).** GCMouse raw input, native fullscreen management, cursor confinement, CrossOver 26.3 display/painting fixes.
- **Graphics routing (0048–0053, 0060–0062, 0093–0100, 0103–0114, 0122–0125, 0130–0131).** Routes D3D to Mullion (DXMT fork), DXVK or VKD3D-Proton per game, plus MetalFX/DLSS plumbing, MoltenVK tuning and wined3d-on-MoltenVK. Most of this is MetalSharp product policy and mostly not wanted for mproton (we use KosmicKrisp, not MoltenVK).
- **Launchers and games (0020–0024, 0040, 0042–0045, 0054, 0056, 0058, 0069, 0071, 0075, 0077, 0080–0087, 0102, 0105–0107, 0115, 0119).** Epic/EA/Battle.net/Rockstar compatibility, a native macOS Steam bridge (0042), HideWineExports, per-folder `.metalsharp` launch env. Proton already has its own versions of much of this, so expect overlap with Proton's launcher and anti-cheat groups.
- **Branding/version bumps (0052, 0055, 0059, 0079, 0087, 0129).** Drop.

Of the two, MetalSharp is less useful as a base for mproton. It's wide and product-specific, and it depends on MoltenVK, Mullion and MetalFX. Its 4K-page and x18 approaches are still worth having as a second reference, and so is its no-entitlement fallback mode (relocated KUSER_SHARED_DATA plus load emulation).

### CrossOver

`media.codeweavers.com` is blocked from this sandbox, so the official source tarball wasn't fetched. A third-party GitHub mirror (`t0b1kent/macrunner-wine`, labelled "CrossOver 26.1.0 / Wine 11.0") reports `Wine version 11.0` and contains `server/msync.c`, `dlls/ntdll/unix/msync.c` and `server/inproc_sync.c`. That tells us **CrossOver 26 is on the same wine-11.0 base as Proton 11, and its msync targets 11.0's inproc-sync framework.** For a Proton 11 tree, that's the msync to take, not athei's 11.20 adaptation. This came from an unofficial mirror. Before using it, verify against the official `crossover-sources-26.x` tarball. The ARM64 bits (4K pages, x18, FEX, entitlement) are in the CrossOver *Preview*, and we couldn't confirm whether that source has been published.

### Key topics: what each stack does

| topic | athei | MetalSharp | Proton 11 | note for mproton |
|---|---|---|---|---|
| 4K pages | Required. The loader re-execs with `posix_spawnattr_set_4k_page_size_np`, and ntdll spawns children with it. No 16K fallback. | Default with the entitlement (`WINE_4K_PAGES`). 16K mode with W^X/W+X emulation otherwise. | n/a (Linux 4K) | Take athei's approach: simpler, and 4K is required for Windows semantics and FEX SMC tracking. |
| per-thread TSO | In FEX: `thread_set_x86_64_compat` via `svc` at JIT entry and exit. Hardware TSO. | Software TSO in FEX (on by default since 0078). | Asahi `prctl` TSO added, then **reverted** | No Wine change needed if we use athei's FEX approach. Upstream FEX has no macOS port; carry athei/FEX. |
| x18 | `custom-x18-abi-toggle` entitlement plus a per-thread opt-in (macOS 26.4+) | Loader linked with SDK version 12.0 | n/a | Two working approaches. The SDK-12 trick needs no extra entitlement but relies on a legacy-compat behaviour Apple could drop. Pick one on a Mac. |
| PAGEZERO | Keep the 4 GB PAGEZERO; with the entitlement claim it as the low reserve, plus 4 GB→loader slide area | Same with the entitlement (0018); without it, relocate KUSER_SHARED_DATA | Proton's Linux preloader (`wine64-preloader`) | — |
| `-x86_64_layout_emulation` | **not used** | **not used** | — | Neither stack uses it. The entitlement's replaceable PAGEZERO covers the low-4 GB need. Nothing to port unless a test on a Mac shows a gap. |
| entitlements | `cross-architecture-support` (PAGEZERO, 4K, HW TSO) + `custom-x18-abi-toggle`, in a signed `wine.app` with a provisioning profile; ntdll spawns through `wine.app` | `cross-architecture-support` optional; `scripts/sign-app.sh` | — | Follows the plan. athei's `wine.app` spawn path (2245b85) is needed so child processes keep the entitlements. |
| msync | CrossOver 26 msync, ported to 11.20, on by default | WineForge WFUSync, on by default | fsync (futex) + ntsync, both Linux-only | For Proton 11: CrossOver 26 msync (same base). For Proton 12: athei's 11.20 port. |

## 3. Applying the macOS stacks to Proton

Method (`wine-patches/scripts/try-apply.sh`): cherry-pick each stack commit by commit onto each base and record clean or conflict. "Cumulative" keeps the clean picks and skips conflicts. "Isolated" tests each commit alone on the base. Results are in `wine-patches/triage/*.tsv`.

| stack | base | cumulative clean | isolated clean |
|---|---|---:|---:|
| athei (36) | wine-11.20 | 36 | — |
| athei | WineHQ master | 27 | 26 |
| athei | vanilla wine-11.0 | 14 | 14 |
| athei | Proton `proton_11.0` | 12 | 12 |
| MetalSharp (131) | WineHQ master | 108 | 68 |
| MetalSharp | vanilla wine-11.0 | 65 | 52 |
| MetalSharp | Proton `proton_11.0` | 47 | 42 |

(MetalSharp's isolated numbers are lower because its patches build on each other.)

### Conflicts caused by Proton (clean on wine-11.0, conflict on Proton)

athei, all four resolved by hand on branch `mproton/athei-on-proton` (series in `wine-patches/athei-on-proton-11.0/`):

| athei patch | file | why | resolution |
|---|---|---|---|
| `39d1ebc` arm64 macOS address space | `configure` | Proton deletes the generated `configure` (`c880d1ca039`) | Drop the hunk; the `configure.ac` change applies |
| `f0cc31b` makedep resources | `tools/makedep.c` | Proton's "modules can have a different 64-bit name" changed `output_module()`'s locals | Merge declarations (`module` + `res_arch`) |
| `5638e0a` load compatdb.so | `dlls/ntdll/unix/loader.c` | Proton's glibc `mallopt(M_PERTURB, 0)` HACK sits at the same insertion point in `start_main_thread()` | Keep both: `mallopt` then `load_compatdb()` (and `#ifdef` the mallopt for darwin, see below) |
| `0564227` APC callback data context | `dlls/ntdll/signal_arm64ec.c` | Proton's "FEX specific AVX support for arm64ec" switched the APC dispatcher to `ARM64EC_NT_XCONTEXT` | Use Proton's extended context and set `callback_data->ContextRecord` to it |

MetalSharp (isolated), 11 patches: 0001 and 0052 (`configure`, same reason as above), 0004 (`wine.inf.in`, Proton's FEX dll-name registry defaults), 0006 (`virtual.c`), 0020 (`kernelbase/process.c`, Proton launcher hacks), 0022 (`services.c`, Proton's FEX env passthrough), 0085/0086 (`ntdll/unix/file.c`), 0092 (`win32u/dce.c`), 0121 (`win32u/d3dkmt.c`), 0126 (`signal_arm64ec.c`, the same Proton AVX context change).

### Conflicts caused by base drift (wine-11.0 → 11.20)

These 20 athei patches conflict on vanilla wine-11.0 as well as on Proton, so Proton isn't the cause:

| area | athei patches | what moved upstream between 11.0 and 11.20 |
|---|---|---|
| ntdll virtual memory | `7d58e47`, `e8e8cc4`, `2245b85` | 67 commits to `virtual.c`, including `1cae1f53` "Merge the reserved areas and free ranges arrays" (2026-10-13) and the top-down reserve change. athei's W^X, `MAP_JIT` and low-reserve code is written against that layout. **This is the core of the macOS port and the main reason not to backport to 11.0.** |
| winemac.drv / win32u client surfaces | `acb1029`, `5a1a2fc`, `23f38ac`, `1114ea6`, `941b2eb` | 103 commits to winemac.drv, plus the win32u `client_surface` API (e.g. `ec23c074`, `1a1d1f3f`) |
| win32u OpenGL on EGL | `17c1044`, `083e30b`, `2831776`, `17a2615` | 120 commits to `win32u/opengl.c` (EGL device and display rework) |
| msync | `7134955` | inproc-sync API changes (`linux_wait_objs` with `WAIT_TYPE`, ntsync mutex list). On Proton there's also a *structural* overlap: Proton's fsync is another backend in the same ops table. |
| wined3d Vulkan / vkd3d | `553ac1b`, `9eb7893`, `4156c72` | wined3d Vulkan churn; bundled vkd3d 2.1 |
| misc | `713c1db` (6 files), `324cd7b` (Makefile.in), `df5ee72` (thread.c), `5959a04` (dnsapi) | context drift |

On WineHQ master, 9 athei patches still conflict: the two big ntdll ones (`7d58e47`, `e8e8cc4`), winemac (`5a1a2fc`, `1114ea6`, `941b2eb`), wined3d (`553ac1b`, `9eb7893`), `713c1db`, and msync (only in the generated `server_protocol.h`, which is trivial: re-run `make_requests`). MetalSharp's 23 conflicts on master are concentrated in `virtual.c` (0018/0019/0117/0118), `ntdll/unix/env.c` (its per-game env policy) and kernelbase launchers. So by Proton 12, the macOS port will mostly need ordinary rebase maintenance, not backporting.

### Darwin build blockers in Proton's tree

Found by reading the code, not by compiling for darwin. Each fails on a darwin host as written:

1. `dlls/ntdll/unix/sync.c:64` and `server/inproc_sync.c:39` include `ntsync_tmp.h` unconditionally, and it includes `<linux/types.h>`. Fix: guard with `#ifdef __linux__` (the `#ifdef NTSYNC_IOC_*` users already cope with it missing).
2. `dlls/ntdll/unix/fsync.c`: `<linux/futex.h>` is guarded, but `struct futex_waitv`, `FUTEX_32` and `syscall(__NR_futex…)` are not. `server/fsync.c` `futex_wake()` has the same problem. Fix: build fsync only on Linux (`#ifdef __linux__` around the body, stub `do_fsync()` returning 0).
3. `dlls/ntdll/unix/loader.c:41`: `#include <malloc.h>`, and `mallopt(M_PERTURB, …)` at lines 2518 and 2528 (`HACK: Zero memory from glibc.malloc`). Neither exists on macOS. Fix: `#ifdef __GLIBC__`.
4. `dlls/amd_ags_x64/unixlib.c:34`: `#include <xf86drm.h>` with `DRM_LIBS` and no configure check. **Also fails on this Linux box (no libdrm-dev), see below.** Fix: a `WINE_PACKAGE_FLAGS(DRM,…)` check and building the unixlib only when found, as protontts does with Piper.
5. `dlls/protontts/unixlib.c:30`: `<linux/limits.h>`. Built only with libpiper; use `<limits.h>`.

Runtime-only Linux paths that fail gracefully (`/proc/self/status`, `/proc/self/cmdline`, `/sys/class/drm`, `/sys/devices/system/cpu`, ftrace's `/sys/kernel/tracing`), and paths that are already guarded (`USE_UFFD_WRITEWATCH`, `HAVE_MEMFD_CREATE`, `MAP_FIXED_NOREPLACE`, `HAVE_SCHED_SETAFFINITY`, `SO_PEERCRED`, xattr with Apple signatures, `signal_x86_64.c` seccomp), aren't blockers.

### Linux build check of the combined tree

Tree: Proton `proton_11.0` + the 16 carried athei patches (`wine-patches/athei-on-proton-11.0/`), with generated files regenerated as Proton's `Makefile.in` does (`autoreconf`, `make_requests`, `make_vulkan -x vk.xml -X video.xml`, `make_specfiles`).

Toolchain: llvm-mingw couldn't be downloaded (GitHub release assets are blocked here). Wine's `--with-mingw=clang` path was used instead, with Ubuntu's clang-20 + lld-20 (`-target aarch64-windows` / `arm64ec-windows`). Host: x86_64 Ubuntu 24.04.

```
configure --enable-archs=aarch64,arm64ec --with-mingw=clang --disable-tests \
  --without-x --without-wayland --without-gstreamer --without-vulkan --without-freetype
make -k
```

BUILD_RESULT_PLACEHOLDER

## 4. Recommended rebase order

The plan stays option A, sequenced by base version rather than by trying to land everything on Proton 11:

1. **Now: macOS core on WineHQ master, not on Proton.** Rebase athei's stack onto master (27/36 clean; the 9 conflicts are listed above). This is the tree milestone 1 brings up on a Mac. Pin it to the same FEX (`athei/FEX` `macos-patched`).
2. **Darwin-proofing patches for Proton's tree** (the 5 blockers above). They're small and independent of the macOS stack, and worth sending upstream to Valve. Apply them first in any Proton-based tree.
3. **When Proton 12 branches from wine-12.0**, rebase onto it in layers, from low to high level. Each layer should build on darwin before the next one goes on:
   1. build system and loader: athei `39d1ebc`, `f0cc31b`, `2245b85` (bundle and spawn); drop Proton's `wine64-preloader` and `/run/host` commits;
   2. ntdll memory and signals: athei `7d58e47`, `e8e8cc4`, `f1fe4a2`, ARM64EC fixes (`8d91fb6`, `0564227`, `df5ee72`), then Proton's ARM64/FEX group on top;
   3. sync: athei msync (`7134955`) next to Proton's fsync, both behind the inproc framework; port Proton's per-game fsync knobs;
   4. compatdb hook (`5638e0a`), mapped onto Proton's `hacks_init()` so one per-game mechanism remains;
   5. winemac.drv and win32u (athei 8–10, 19–20, 26–33), then decide what to do with Proton's win32u fshack;
   6. wined3d and vkd3d fixes, then the remaining small fixes;
   7. replacements for the winex11 behaviours Proton depends on (raw input, cursor clip, fullscreen), using MetalSharp 0038/0039/0127 as references.
4. **Only if a working Proton 11 tree is needed earlier:** use CrossOver 26 (wine-11.0) as the macOS source instead of athei/MetalSharp, since it's on the same base. That would need a fresh apply run against the official CrossOver 26 tarball.
5. Leave MetalSharp's graphics-routing, MetalFX, DLSS and branding patches out. Take individual fixes from it (W+X store emulation, the no-entitlement fallback, launcher fixes) only when we hit the same bug.

## What this could not verify

- **Nothing was built or run for darwin.** There's no Mac and no macOS SDK here. The darwin build blockers come from reading the code, and the darwin unix side (loader, ntdll.so, winemac.drv, msync, wineserver) was never compiled. Whether the 16-patch combined tree works on macOS is unknown.
- Hand-resolved conflicts were checked only by the Linux compile of the touched files. In particular, the `signal_arm64ec.c` APC merge needs a test on ARM64.
- The entitlement claims (4K pages, replaceable PAGEZERO, `thread_set_x86_64_compat`, the x18 toggle, x18 preservation with SDK 12) come from the athei/MetalSharp docs and code, not from testing.
- CrossOver: the official source wasn't reachable; the "CrossOver 26 = wine-11.0 with msync" finding comes from a third-party mirror. Whether CrossOver Preview's ARM64 source is published is unknown.
- `classify-proton.py` is a heuristic over subjects and paths. Spot checks looked right, but individual commits will be misfiled. The TSV is meant to be corrected by hand.
- The build check used plain clang-20 instead of llvm-mingw, and left out X11, Wayland, GStreamer, Vulkan and FreeType. Unix-side code behind those options wasn't compiled.

## Reproducing

```bash
wine-patches/scripts/reproduce.sh /path/to/workdir
```

It clones Proton Wine and adds the WineHQ, athei and MetalSharp remotes at the pinned SHAs. Then it runs the classifier and the full apply matrix, rebuilds the combined tree from `wine-patches/athei-on-proton-11.0/`, and runs the Linux build check. Files:

- `wine-patches/scripts/classify-proton.py`: grouping and classification of Proton commits
- `wine-patches/scripts/try-apply.sh`: per-commit apply test (cumulative or isolated)
- `wine-patches/triage/proton-commits.tsv`, `proton-groups.txt`: per-commit classification and the group summary
- `wine-patches/triage/{athei,ms,iso-athei,iso-ms}-on-{proton,wine11.0,wine11.20,master}.tsv`: apply results
- `wine-patches/athei-on-proton-11.0/`: the 16 athei patches as carried on `proton_11.0` (LGPL-2.1+, from `athei/wine`; authorship kept)
