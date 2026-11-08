# Display modes on the Mac

What a Windows game sees of the display under mproton, and why a game's resolution list can be empty.

## The probe: `tests/displaymodes`

`tests/displaymodes/displaymodes.cpp` prints what a game can query:

- GDI: `EnumDisplayDevicesW` (adapters and monitors); `EnumDisplaySettingsW` (the current mode, the registry mode and every mode, also with `EDS_RAWMODE`); monitor rects from `EnumDisplayMonitors` / `GetMonitorInfoW`; `GetSystemMetrics`.
- CCD: `QueryDisplayConfig` paths and modes (refresh rate as a fraction), and `DisplayConfigGetDeviceInfo` source and target names.
- DXGI: adapters, outputs (`GetDesc`), `GetDisplayModeList` and `GetDisplayModeList1` for R8G8B8A8 / B8G8R8A8 (UNORM and SRGB), R10G10B10A2 and R16G16B16A16_FLOAT, and `FindClosestMatchingMode` for the desktop size. It does this once through `CreateDXGIFactory1` and once through a D3D11 device's adapter, which is the path Unity takes.

The probe calls `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` first, like a Unity player (`-nodpi` skips it). The launcher doesn't pass stdout through, so the probe writes to the file named after `-o`.

```bash
tests/displaymodes/build.sh     # out/displaymodes-x86_64.exe, out/displaymodes-i686.exe
tests/displaymodes/run.sh       # each arch x dxmt (default), dxvk, wined3d -> out/<arch>-<backend>.txt
```

`run.sh` uses a scratch prefix (`~/mproton-prefixes/displaymodes`). Any `PROTON_*` variable you set is passed through to the launcher; for example `PROTON_EMULATE_MODESET=1 tests/displaymodes/run.sh`.

## What it shows (M2 MacBook Air, built-in panel, 1470x956 desktop, 2026-11-07)

All six runs agree: x86_64 and i686, each with DXMT, DXVK and wined3d.

- **GDI:** 42 modes: 14 sizes x 32/16/8 bpp, all at 60 Hz. The sizes are 960x600, 1024x640, 1024x666, 1280x800, 1280x832, 1470x918, 1470x956 (current), 1710x1068, 1710x1112, 1920x1200, 2048x1280, 2048x1332, 2560x1600 and 2560x1664. They are macOS's scaled modes for this panel: 16:10, plus the notch-height variants. **None is 16:9.**
- **CCD:** one path, refresh 60/1, target "Wine Monitor", source 1470x956.
- **DXGI:** 14 modes for every 8- and 10-bit format at 60000/1000 Hz, through both the factory and the device. FP16 has 0, which is expected because no 64 bpp mode exists. `FindClosestMatchingMode(1470x956)` finds 1470x956.

So winemac.drv, win32u, DXMT and DXVK all return a sane, non-empty list. There are no 0 Hz or fractional refresh rates and no wrong bpp. **The enumeration is not broken.**

## Why Castlevania: Belmont's Curse shows no resolution

Unity's docs say that `Screen.resolutions` "returns all full-screen resolutions that the monitor supports" ([2022.3 docs](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Screen-resolutions.html)). The player is closed source, so the exact Win32/DXGI calls behind it aren't documented. The probe shows that every candidate API returns the same 14 sizes.

The empty list comes from the game's own filter. The IL2CPP metadata (`Castlevania Belmont's Curse_Data/il2cpp_data/Metadata/global-metadata.dat`) names these members, in this order, in its `VideoSettings` class:

- `Is16By9`
- `IsSameRatio(r1, r2)`
- `UpdateSupportedResolutions`, with a closure that captures `nativeResolution`
- a serialized `supportOnly16By9` flag
- `_16By9Resolutions`

The menu offers only 16:9 resolutions, and macOS offers none.

What is inference and what isn't: the member names are read from the metadata, not decompiled. The exact branch in `UpdateSupportedResolutions` is not confirmed. The fix below confirms the cause, because adding 16:9 sizes to the list fills the menu.

On a Windows PC the GPU driver lists many standard sizes (1280x720, 1920x1080 and so on, using GPU scaling) on any panel, so the list isn't empty there. A 16:10 Steam Deck goes through Proton's virtual mode list, which has 16:9 sizes.

## The fix: emulated modesetting

Wine's win32u already has the right mode list. With `EmulateModeset` on, `get_virtual_modes` (`dlls/win32u/sysparams.c`) replaces the host's modes with a standard list that includes 4:3, 16:9, 16:10, 3:2, 21:9 and 5:4 sizes up to the host mode. On this panel that is:

640x480, 800x600, 960x540, 960x600, 1024x640, 1024x666, 1024x768, **1280x720**, 1280x800, 1280x832, 1440x900, 1470x918, 1470x956.

Mode changes are emulated (they never touch the Mac's display), and the result is scaled to the window.

The setting is the registry value `HKCU\Software\Wine\X11 Driver\EmulateModeset`. A per-exe `AppDefaults\<game>.exe\X11 Driver` key **does not work** for the mode list. The list is built and cached by whichever process refreshes the display cache (explorer.exe), not by the game. The probe confirmed this: the per-exe key changed nothing, and the prefix-wide key did. Setting the prefix-wide key changes the prefix for good, so mproton uses an environment variable instead:

- **Wine series:** `mproton: win32u: Let WINE_EMULATE_MODESET override the EmulateModeset setting.` `sysparams_init` reads `WINE_EMULATE_MODESET` after the registry. Every process of a launch has it, explorer.exe included.
- **Launcher:** compat option `emulatemodeset` sets `WINE_EMULATE_MODESET=1`. It is on by default for 4231820. `PROTON_EMULATE_MODESET=1` turns it on for any game and `=0` turns it off.

Checked in the game (2026-11-07): the menu shows 1280x720. That is its only entry, so the arrows don't change it. The emulated 960x540 isn't offered, presumably because the game has a minimum size; this is inferred, not read from its code. 1280x720 is the largest 16:9 size, because the virtual list stops at the 1470x956 desktop. Larger 16:9 sizes (1920x1080) would need the list to go up to the panel's 2560x1664 pixel modes, or Wine's Retina mode.

With the rebuilt bundle and `PROTON_EMULATE_MODESET=1`, the probe lists the 13 sizes above in GDI and DXGI, on both x86_64 and i686.

## Upstream

- **athei/wine (or WineHQ):** the `WINE_EMULATE_MODESET` override is small and self-contained. The reason to send it: the setting only works prefix-wide, because the mode cache is built in explorer.exe, so a launcher can't turn it on for a single game. Proton handles per-game switches with environment variables in the same way.
- **Alternatively, for winemac.drv:** add the standard sizes to the host mode list as virtual modes, as win32u does for single-mode hosts, and keep real CoreGraphics mode switches for the native modes. That is a bigger change. Whether emulated modesetting should be mproton's default for every game, given that macOS games never get real mode switches anyway, is an open question and needs testing on more games.
- **DXMT:** nothing to send for this bug. One unrelated detail from reading its source: `GetMonitorFormatBpp` knows `B8G8R8X8_UNORM`, but that format has no `MTL_DXGI_FORMAT_BACKBUFFER` flag, so `GetDisplayModeList(B8G8R8X8_UNORM)` returns 0 modes. This was not tested here.
