# mproton

**Run Windows Steam games on Apple Silicon Macs, the way Proton runs them on Linux.**

mproton brings the pieces that make Valve's [Proton](https://github.com/ValveSoftware/Proton) work on the Steam Deck (its Wine patches, its Steam bridge and its launcher) to macOS on Apple Silicon. It builds on athei's [mach9](https://github.com/athei/mach9) ARM64 Wine stack: Wine runs natively on arm64, x86 game code runs under [FEX](https://github.com/FEX-Emu/FEX), and Direct3D goes to Metal through DXMT, or to Vulkan (KosmicKrisp) through DXVK and vkd3d-proton.

> **Status: experimental.** It runs real games on the author's M2, but it's early. Signed, notarized builds are on the [Releases](https://github.com/jesscarter/mproton/releases) page.

## What works

The games tried so far, with what worked and what didn't, are listed in [docs/running.md](docs/running.md#games-tested).

**The Steam side works too:** lsteamclient talks to the running Mac Steam client from both 64-bit and 32-bit games. A test program unlocked, stored and cleared a Steam achievement through it on both.

**Not working yet, or unknown:**
- Games with anti-cheat. macOS has no equivalent of the Linux anti-cheat runtimes Proton uses, so online games protected by EAC, BattlEye or VAC are out of scope for now.
- D3D9 and D3D12 games in practice (they pass small tests but no real game has been tried).
- A known set of correctness-test failures: some exception delivery cases and one Vulkan copy test ([results](results/ladder-2026-11-07-mproton/summary.md)).
- Starting games from Steam's own Play button. For now you download the Windows build and start it with mproton (below).

## Requirements

- An Apple Silicon Mac (M1 or newer) with **macOS 26.4 or newer**.
- Steam for Mac, signed in, for games that use the Steam API.
- Python 3, which comes with Apple's Command Line Tools. If they're missing, mproton Library offers to install them (or run `xcode-select --install`).

That's all you need to use a release. Building mproton yourself needs more, see [Building it yourself](#building-it-yourself).

## Getting started

1. **Download mproton.** Get `mproton-<version>-macos-arm64.tar.xz` from [Releases](https://github.com/jesscarter/mproton/releases) and double-click it to unpack. You get a folder holding **mproton Library** and a `wine` folder. Keep the two together: move the whole folder somewhere permanent (for example into your Applications folder). Everything is signed and notarized, so macOS opens it without warnings.

   v0.1.0 has only the `wine` folder. For it, move `wine` to `~/mproton-wine`, clone this repository and start the library with `mproton/library/mproton-library`.

2. **Open mproton Library.** It's a small app with your Windows games, Play and Stop buttons, logs and per-game settings ([library/README.md](library/README.md)).

3. **Get a game's Windows build.** In mproton Library, press **Get a Windows game**, search for a game you own, and paste the `download_depot` lines it shows into Steam's console (the **Steam console** button opens it). This downloads only that game's Windows build, so games you have as Mac builds aren't touched. By hand, the console command is:

   ```
   download_depot <appid> <depotid>
   ```

   Depot IDs are also listed on SteamDB or by `https://api.steamcmd.net/v1/info/<appid>`. The other way is the **Windows downloads** switch in mproton Library, which makes Steam download Windows builds of every game, including games that have a Mac build.

4. **Play it:** pick the game in mproton Library and press **Play**. Or run a game by hand with the launcher: see [docs/running.md](docs/running.md).

## Building it yourself

Only needed if you want to change mproton itself:

- **Full Xcode**, Homebrew, Rust and a few build tools: see [docs/building.md](docs/building.md#prerequisites).
- **A paid Apple Developer Program membership.** Wine's loader needs Apple's `com.apple.developer.cross-architecture-support` entitlement, which only works through a provisioning profile from a paid team. A free Personal Team doesn't qualify ([why](docs/m0-baseline.md#signing-is-a-hard-requirement)). The only alternative is an ad-hoc signature with SIP disabled and an AMFI boot-arg, which weakens your Mac's security a lot. Use it only on a dedicated test machine.

```bash
scripts/setup.sh
LOADER_PROFILE=~/signing/mproton-loader.provisionprofile build/mproton-build.sh
```

The full guide is [docs/building.md](docs/building.md). `build/mproton-release.sh <tag>` turns a bundle signed with a Developer ID profile into a notarized release, with mproton Library.app built, signed and notarized beside it.

## How it fits together

```
Windows game (x86_64 or i686)
 ├─ x86 code ─────────────► FEX (in Wine's ARM64EC / WoW64 layer)
 ├─ Direct3D 10/11 ───────► DXMT ─► Metal          (or DXVK ─► Vulkan)
 ├─ Direct3D 12 ──────────► vkd3d-proton ─► Vulkan (KosmicKrisp) ─► Metal
 ├─ steam_api ─► steamclient(64).dll stand-in ─► lsteamclient ─► Mac steamclient.dylib
 └─ everything else ──────► Wine 11.20 (athei's arm64 tree + ~730 Proton patches), native arm64
```

The repository doesn't fork Proton or Wine. It pins upstream commits and keeps only mproton's changes on top: a Wine patch series, patches to mach9's build scripts, Proton's Steam components ported to macOS, and its own build, launcher, tests and tools.

## Repository layout

| Path | What it is |
|---|---|
| `build/` | `mproton-build.sh`, which builds the whole bundle ([docs/building.md](docs/building.md)); upstream pins; patches to mach9 |
| `wine/` | The Proton Wine patch series on athei's arm64 Wine, `apply.sh`, per-commit status ([docs/wine-tree.md](docs/wine-tree.md), [wine/PORTING.md](wine/PORTING.md)) |
| `steam/` | Proton's `lsteamclient` and `steam.exe` ported to arm64 macOS, the `steamclient(64).dll` stand-ins, ABI checks and tests ([steam/README.md](steam/README.md)) |
| `launcher/` | `mproton`, a macOS port of Proton's launcher script ([launcher/README.md](launcher/README.md)) |
| `library/` | mproton Library, the local Play/Stop app ([library/README.md](library/README.md)) |
| `graphics/dxvk/` | Reproducible DXVK build (ARM64X + i386) and KosmicKrisp audit ([docs/graphics-dxvk.md](docs/graphics-dxvk.md)) |
| `tests/` | The launcher's pytest suite, and `tests/ladder/`: small Windows programs checking threads, exceptions, memory ordering, D3D9/11/12, OpenGL and Vulkan ([docs/testing.md](docs/testing.md)) |
| `bench/` | mpbench, a CPU / D3D11 / D3D12 / Vulkan benchmark for every architecture |
| `results/` | Test and benchmark summaries ([results/README.md](results/README.md)) |
| `docs/` | Design notes: [port plan](docs/macos-port-plan.md), [Steam integration](docs/steam-integration.md), [Wine patch triage](docs/wine-patch-triage.md), milestone write-ups |

## Credits

mproton stands on other people's work:
- **Valve's Proton:** the Wine patches, lsteamclient, steam_helper and the launcher.
- **athei's mach9 and arm64 Wine:** the macOS ARM64 Wine stack that everything builds on.
- **The Wine project, FEX, DXMT, DXVK, vkd3d-proton and Mesa's KosmicKrisp.**
- **The Kaon and NotProton projects:** research on Steam's macOS compatibility hooks.

mproton is an independent project. It is **not affiliated with or endorsed by Valve Corporation**. Proton and Steam are trademarks of Valve Corporation.

## License

mproton's own code is under the BSD 3-Clause License ([LICENSE](LICENSE)). Parts that come from other projects keep their licences:

- `steam/` (lsteamclient, steam_helper) and `launcher/` (a port of Proton's `proton` script): Valve's Proton licence, BSD 3-Clause ([steam/LICENSE.proton](steam/LICENSE.proton), [launcher/LICENSE.proton](launcher/LICENSE.proton)); `steam/lsteamclient/` also carries its own [LICENSE](steam/lsteamclient/LICENSE).
- `wine/patches/` and `wine-patches/`: patches to Wine, under Wine's LGPL 2.1 or later, by their authors as recorded in each patch.
- `build/mach9-patches/`: patches to athei's mach9 scripts, under mach9's zlib-style licence.
