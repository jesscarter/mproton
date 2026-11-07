#!/usr/bin/env python3
"""Group Proton's Wine commits (base..tip) and assign a macOS-ARM64 category.

usage: classify-proton.py <wine-repo> <base> <tip> <out-dir>
writes <out-dir>/proton-commits.tsv (one row per commit) and prints a summary.

Rules are matched in order against the subject (s) and the list of touched
files (f, space-joined); the first match wins.  Categories:
  portable   - PE-side / game fixes / platform-neutral unix code; keep as is
  linux      - Linux-only: drop or keep behind #ifdef on macOS
  mac-equiv  - needs a macOS equivalent (winemac.drv, msync, IOHID, ...)
  conflict   - portable in intent, but touches code the macOS ARM64 stack rewrites
"""
import re, subprocess, sys, collections

RULES = [
 # (group, category, subject-regex, files-regex)
 ("fsync / ntsync / in-process sync", "mac-equiv", r"fsync|ntsync|esync|inproc.?sync|futex", r"fsync|ntsync"),
 ("gamescope / Steam Deck", "linux", r"gamescope|steam ?deck|steamos|\bdeck\b", None),
 ("winewayland.drv", "linux", r"wayland", r"winewayland"),
 ("fullscreen hack (fshack, win32u/opengl32 side)", "conflict", r"fshack|fs ?hack|fullscreen hack", None),
 ("Linux tracing / debug (ftrace, gpuvis, uffd, memfd)", "linux", r"ftrace|gpuvis|uffd|userfault|memfd|eventfd|ppoll|glibc|LD_PRELOAD|CLOCK_BOOTTIME|/proc/|pressure.?vessel|steam runtime", r"uffd"),
 ("ARM64 / ARM64EC / FEX / WoW64", "conflict", r"arm64|aarch64|\bfex\b|arm64ec|wow64|chpe|\bsve\b|\bTSO\b|xtajit|BTCpu|cooperative suspend", r"^dlls/wow64/"),
 ("winex11.drv (window mgmt, input, XInput2, Vulkan surfaces)", "mac-equiv", r"winex11|\bx11\b|xinput2|xrandr|xcomposite|_NET_WM", r"dlls/winex11\.drv/"),
 ("winebus / HID / Steam Input / controllers", "mac-equiv", r"winebus|hidclass|hidraw|udev|\bsdl\b|steam input|dinput|xinput|windows\.gaming\.input|hidp?", r"dlls/(winebus\.sys|hidclass\.sys|dinput|xinput|windows\.gaming\.input|hid)/"),
 ("audio drivers (winepulse/winealsa) & mmdevapi", "mac-equiv", r"winepulse|winealsa|pulse|alsa|mmdevapi", r"dlls/(winepulse\.drv|winealsa\.drv|mmdevapi)/"),
 ("winevulkan / win32u Vulkan / OpenXR / vrclient", "conflict", r"vulkan|vk_|openxr|openvr|vrclient|wine_vk", r"dlls/winevulkan/|dlls/win32u/vulkan\.c|dlls/vulkan-1"),
 ("OpenGL (opengl32 / win32u opengl / wined3d)", "conflict", r"opengl|wgl|\bgl\b|wined3d|\begl\b", r"dlls/(opengl32|wined3d)/|dlls/win32u/opengl\.c"),
 ("Proton integration (steam.exe, proton hooks, lsteamclient, env vars)", "portable", r"^(HACK: )?(proton|steam)[:/ ]|steamclient|steam\.exe|SteamGameId|PROTON_|WINESTEAM", None),
 ("prefix / registry defaults (wine.inf, wineboot, fonts)", "portable", r"wine\.inf|wineboot|font|registry entr", r"loader/wine\.inf"),
 ("loader / preloader / build system (configure, makedep, winebuild, winegcc)", "conflict", r"^(HACK: )?(fixup! )?(loader|configure|makedep|winebuild|winegcc|tools|gdbinit)\b", r"^(loader/main\.c|loader/preloader)"),
 ("wineserver core", "conflict", r"^(HACK: )?(fixup! )?(Revert \")?server\b", r"(^| )server/"),
 ("ntdll unix side (virtual memory, signals, sync, process)", "conflict", r"^(HACK: )?(fixup! )?(Revert \")?(ntdll|kernelbase, ntdll|ntdll, kernelbase)\b", r"dlls/ntdll/unix/(virtual|signal_|loader|thread|server|sync|process|system)"),
 ("win32u (windows, messages, display, d3dkmt)", "conflict", r"^(HACK: )?(fixup! )?(Revert \")?(win32u|user32|win32)\b", r"dlls/win32u/"),
 ("multimedia (winegstreamer, mf*, media-converter, quartz, wm*)", "portable", r"gstreamer|media.?converter|^(HACK: )?(mf|mfplat|mfreadwrite|mfmediaengine|mfsrcsnk|qasf|quartz|wmvcore|wmvdecod|msauddecmft|winedmo|wm_reader|amstream|strmbase|iyuv|mciavi|dsound|xaudio|xactengine)", r"dlls/(winegstreamer|mf|mfplat|mfreadwrite|winedmo|quartz|qasf|wmvcore)/"),
 ("GPU vendor libs (amd_ags, atiadlxx, amdxc, nvapi, dxgi vendor)", "portable", r"amd_ags|atiadlxx|amdxc|nvapi|nvcuda|nvml|dxcore", r"dlls/(amd_ags_x64|atiadlxx|amdxc64)/"),
 ("speech / TTS (windows.media.speech, protontts, Vosk, Piper)", "portable", r"speech|protontts|vosk|piper|sapi", r"dlls/(windows\.media\.speech|protontts|sapi)/"),
 ("networking (ws2_32, nsiproxy, iphlpapi, winhttp, wininet)", "portable", r"ws2_32|nsiproxy|iphlpapi|winhttp|wininet|dnsapi|ping|icmp|sock", r"dlls/(ws2_32|nsiproxy\.sys|iphlpapi|winhttp|wininet)/"),
 ("d3dx / d3dcompiler / ddraw / dxgi PE side", "portable", r"d3dx|d3dcompiler|ddraw|dxgi|d3d1[0-2]|d3d9|d3d8|dxdiag", None),
 ("anti-cheat / launchers (EAC, BattlEye, EA, Ubisoft, CEF)", "portable", r"\beac\b|easyanticheat|battleye|belauncher|\bea\b|ubisoft|uplay|cef|origin|launcher|epic|rockstar", None),
]
# Manual overrides for commits whose platform dependence the rules can't see.
OVERRIDES = {
 "e8f45bef0e4": ("Linux tracing / debug (ftrace, gpuvis, uffd, memfd)", "linux"),  # wine64-preloader: Linux preloader
 "3e5eb3bceba": ("Linux tracing / debug (ftrace, gpuvis, uffd, memfd)", "linux"),
 "4027aab4bf7": ("Linux tracing / debug (ftrace, gpuvis, uffd, memfd)", "linux"),  # /run/host = pressure-vessel
 "579584c8cf7": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),  # SVE xstate: no SVE on M1-M3, Linux sigcontext
 "2a8c650fb47": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),  # SVE predicate reg in Linux signal frame
 "abd586c858c": ("Linux tracing / debug (ftrace, gpuvis, uffd, memfd)", "linux"),  # x86_64-host syscall user dispatch
 "c1b93e22bfd": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),  # Asahi prctl TSO (reverted later)
 "06da2f4afc6": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),
 "28c7d11252b": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),  # Linux-kernel unaligned atomics (reverted)
 "c6e7e9afa91": ("ARM64 / ARM64EC / FEX / WoW64", "linux"),
}
FALLBACK = ("other PE-side game / app fixes", "portable")

def main():
    repo, base, tip, out = sys.argv[1:5]
    log = subprocess.run(["git","-C",repo,"log","--no-merges","--reverse","--format=@@@%h%x09%an%x09%s","--name-only",f"{base}..{tip}"],
                         capture_output=True, text=True, check=True).stdout
    commits = []
    for chunk in log.split("@@@")[1:]:
        lines = [l for l in chunk.splitlines() if l.strip()]
        h, an, s = lines[0].split("\t", 2)
        commits.append((h, an, s, lines[1:]))
    groups = collections.OrderedDict()
    rows = []
    for h, an, s, files in commits:
        fj = " ".join(files)
        for g, cat, sr, fr in RULES:
            if (sr and re.search(sr, s, re.I)) or (fr and re.search(fr, fj)):
                break
        else:
            g, cat = FALLBACK
        if h[:11] in OVERRIDES: g, cat = OVERRIDES[h[:11]]
        hack = bool(re.search(r"\bHACK\b", s))
        unix = any(re.search(r"/unix/|unixlib|^server/|^loader/|\.drv/|^configure", f) for f in files)
        rows.append((h, g, cat, "HACK" if hack else "", "unix" if unix else "pe", an, s))
        groups.setdefault(g, [cat, 0, 0, 0])
        groups[g][1] += 1; groups[g][2] += hack; groups[g][3] += unix
    with open(f"{out}/proton-commits.tsv","w") as fh:
        fh.write("commit\tgroup\tcategory\thack\tside\tauthor\tsubject\n")
        for r in rows: fh.write("\t".join(r)+"\n")
    print(f"{'group':70} {'cat':10} {'n':>5} {'HACK':>5} {'unix':>5}")
    for g,(cat,n,hk,ux) in sorted(groups.items(), key=lambda kv:-kv[1][1]):
        print(f"{g[:70]:70} {cat:10} {n:5} {hk:5} {ux:5}")
    print("total", len(rows))
    c = collections.Counter(r[2] for r in rows); print(dict(c))

main()
