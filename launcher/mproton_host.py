"""Host-platform abstraction for the mproton launcher.

The launcher (``mproton``) is a fork of Valve's ``proton`` script.  To keep the
diff from upstream small and rebasable, every Linux-vs-macOS decision lives
here and the launcher only asks questions like ``g_host.uses_pressure_vessel``.

Nothing in this module is evaluated at import time: ``HostPlatform.detect()``
queries ``platform``/``os`` when called, so tests can patch those first.
"""

import errno
import os
import platform
import shutil
import sys

LINUX = "Linux"
DARWIN = "Darwin"

# platform.machine() spellings -> the names Proton/Wine use.
# macOS reports "arm64" where Linux reports "aarch64".
_MACHINE_ALIASES = {
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "x86_64": "x86_64",
    "amd64": "x86_64",
    "AMD64": "x86_64",
    "i386": "i386",
    "i686": "i386",
}

# Host (unix-side) library dirs inside files/lib/, per (system, machine).
_HOST_LIB_TRIPLES = {
    (LINUX, "x86_64"): ["x86_64-linux-gnu", "aarch64-linux-gnu", "i386-linux-gnu"],
    (LINUX, "aarch64"): ["x86_64-linux-gnu", "aarch64-linux-gnu", "i386-linux-gnu"],
    (DARWIN, "aarch64"): ["aarch64-apple-darwin"],
    (DARWIN, "x86_64"): ["x86_64-apple-darwin"],
}


def normalize_machine(machine):
    return _MACHINE_ALIASES.get(machine, machine)


class HostPlatform:
    def __init__(self, system, machine, environ=None):
        self.system = system
        self.raw_machine = machine
        self.machine = normalize_machine(machine)
        self.environ = os.environ if environ is None else environ

    @classmethod
    def detect(cls, environ=None):
        return cls(platform.system(), platform.machine(), environ)

    # ---- identity -------------------------------------------------------
    @property
    def is_linux(self):
        return self.system == LINUX

    @property
    def is_darwin(self):
        return self.system == DARWIN

    @property
    def is_arm64(self):
        """True on aarch64 Linux *and* arm64 macOS (FEX hosts)."""
        return self.machine == "aarch64"

    # ---- Steam Linux Runtime / pressure-vessel --------------------------
    @property
    def uses_pressure_vessel(self):
        """Whether the Steam Linux Runtime (pressure-vessel, SLR adverbs) exists.

        There is no runtime container on macOS: the launcher runs directly on
        the host and require_tool_appid is dropped from the tool manifest.
        """
        return self.is_linux

    # ---- dynamic linker -------------------------------------------------
    @property
    def library_path_var(self):
        """Env var used to point the dynamic linker at bundled libs, or None.

        Linux: LD_LIBRARY_PATH, as upstream.

        macOS: None by default.  SIP strips DYLD_* from the environment of any
        protected binary (/bin/sh, /usr/bin/env, system python3), and a
        hardened-runtime-signed wine ignores them without the
        allow-dyld-environment-variables entitlement.  mproton's Wine and
        its dylibs are therefore built with LC_RPATH (@loader_path/...)
        and need no env var.  For unsigned local dev builds,
        MPROTON_DYLD_FALLBACK=1 uses DYLD_FALLBACK_LIBRARY_PATH, which
        (unlike DYLD_LIBRARY_PATH) never shadows system libraries.
        """
        if self.is_darwin:
            if self.environ.get("MPROTON_DYLD_FALLBACK", "0") not in ("", "0"):
                return "DYLD_FALLBACK_LIBRARY_PATH"
            return None
        return "LD_LIBRARY_PATH"

    def host_lib_dirs(self, lib_dir):
        triples = _HOST_LIB_TRIPLES.get((self.system, self.machine),
                                        _HOST_LIB_TRIPLES[(LINUX, "x86_64")])
        return [lib_dir + t for t in triples]

    def steam_media_lib_dirs(self, steam_path):
        """Steam's bundled ffmpeg (video) dirs added to the library path."""
        if self.is_darwin:
            # The macOS client ships no ubuntu12_* tree; Wine's media
            # stack uses our bundled gstreamer/ffmpeg (rpath-linked).
            return []
        return [steam_path + "/ubuntu12_64/video/",
                steam_path + "/ubuntu12_32/video/",
                steam_path + "/steamrtarm64/video/"]

    # ---- synchronization primitives ------------------------------------
    def sync_env(self, compat_config):
        """Return (set, unset) env for Wine's fast sync primitive.

        Linux: fsync (WINEFSYNC) unless 'nofsync'.
        macOS: fsync/ntsync are Linux-only; use msync (Mach semaphores,
        WINEMSYNC) unless 'nomsync'.  The per-game 'nofsync' entries in the
        compat tables exist because the game breaks under a fast sync path,
        so they disable msync as well.
        """
        if self.is_darwin:
            unset = ["WINEFSYNC", "WINEESYNC", "WINENTSYNC"]
            if "nomsync" in compat_config or "nofsync" in compat_config:
                return {}, unset + ["WINEMSYNC"]
            return {"WINEMSYNC": "1"}, unset
        if "nofsync" in compat_config:
            return {}, ["WINEFSYNC"]
        return {"WINEFSYNC": "1"}, []

    # ---- /proc replacements ---------------------------------------------
    def loaded_kernel_modules(self):
        """Set of loaded kernel module names, or None if unknown.

        Only Linux has /proc/modules.  On macOS there is no NVIDIA driver,
        so callers treat None as 'no NVIDIA driver'.
        """
        if not self.is_linux:
            return None
        try:
            with open("/proc/modules") as f:
                return set(line.partition(" ")[0] for line in f.read().splitlines())
        except OSError:
            return None

    def max_map_count(self):
        """vm.max_map_count on Linux; None where the concept does not apply."""
        if not self.is_linux:
            return None
        try:
            with open("/proc/sys/vm/max_map_count", "r") as f:
                return int(f.read())
        except (OSError, ValueError):
            return None

    @property
    def has_nvidia_wine_dlls(self):
        """Whether to probe for the NVIDIA Linux driver's nvngx.dll."""
        return self.is_linux

    @property
    def has_vr(self):
        """OpenVR/OpenXR bridges (vrclient, wineopenxr) are Linux-only builds."""
        return self.is_linux

    # ---- filesystem -----------------------------------------------------
    @property
    def needs_casefold_ioctl(self):
        """ext4 casefold via EXT2_IOC_SETFLAGS is Linux-only.

        APFS/HFS+ volumes are case-insensitive by default; see
        dir_is_case_insensitive() for the warning on case-sensitive ones.
        """
        return self.is_linux

    @staticmethod
    def dir_is_case_insensitive(path):
        """Probe whether path's filesystem folds case (no /proc, no ioctl)."""
        path = path.rstrip("/")
        head, tail = os.path.split(path)
        swapped = tail.swapcase()
        if swapped == tail:
            return None
        try:
            return os.path.samefile(path, os.path.join(head, swapped))
        except OSError:
            return False

    def clonefile_func(self):
        """Return a copy function using APFS clonefile(2) on macOS, else None."""
        if not self.is_darwin:
            return None
        try:
            from ctypes import CDLL, c_char_p, c_int, c_uint32, get_errno
            libc = CDLL(None, use_errno=True)
            clonefile = libc.clonefile
            clonefile.argtypes = [c_char_p, c_char_p, c_uint32]
            clonefile.restype = c_int
        except (OSError, AttributeError):
            return None

        def copyfile_clone(srcname, dstname):
            if clonefile(os.fsencode(srcname), os.fsencode(dstname), 0) == 0:
                return
            err = get_errno()
            if err not in (errno.EXDEV, errno.ENOTSUP, errno.EEXIST, errno.EINVAL):
                raise OSError(err, os.strerror(err), dstname)
            if err == errno.EEXIST:
                os.remove(dstname)
            shutil.copyfile(srcname, dstname)
        return copyfile_clone

    # ---- launching ------------------------------------------------------
    def launcher_adverb(self):
        """Steam Runtime launcher-interface adverb (Linux SLR only)."""
        if self.uses_pressure_vessel and shutil.which("steam-runtime-launcher-interface-0") is not None:
            return ["steam-runtime-launcher-interface-0", "proton"]
        return []

    @property
    def uses_wine_preloader(self):
        """Upstream runs x86_64 Wine through wine-preloader directly.

        Wine on macOS has no separate preloader binary (it is linked into
        the loader), so always go through bin/wine there.
        """
        return self.is_linux

    def default_steam_client_path(self):
        """Fallback for STEAM_COMPAT_CLIENT_INSTALL_PATH.

        On macOS steamclient.dylib (which lsteamclient dlopen()s from
        $STEAM_COMPAT_CLIENT_INSTALL_PATH) lives inside the app bundle in the
        user's Steam dir; this matches NotProton's fallback.
        """
        home = self.environ.get("HOME", "")
        if self.is_darwin:
            return os.path.join(home, "Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS")
        return os.path.join(home, ".local/share/Steam")

    def describe(self):
        return "%s %s (%s)" % (self.system, self.machine, sys.platform)
