"""Fixtures for testing launcher/mproton on Linux with a mocked host platform.

The launcher is a script without a .py suffix, so it is loaded through
SourceFileLoader.  Upstream's vendored filelock is not available here, so a
no-op stand-in is installed when the real package is missing.
"""

import importlib.machinery
import importlib.util
import os
import sys
import types

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
sys.path.insert(0, LAUNCHER)

try:
    import filelock  # noqa: F401
except ImportError:
    class _FileLock:
        def __init__(self, path, timeout=-1):
            self.path = path
        def __enter__(self):
            return self
        def __exit__(self, *exc):
            return False
    sys.modules["filelock"] = types.SimpleNamespace(FileLock=_FileLock)

import mproton_host  # noqa: E402

BUILTIN_MARKER = b"Wine builtin DLL"
PE_ARCHES = ("x86_64-windows", "i386-windows", "aarch64-windows")


def load_launcher():
    loader = importlib.machinery.SourceFileLoader("mproton_launcher", os.path.join(LAUNCHER, "mproton"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def write(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def builtin_dll(path):
    write(path, b"MZ" + b"\0" * 0x3e + BUILTIN_MARKER + b"\0" * 16)


def native_dll(path, tag):
    write(path, b"MZ" + b"\0" * 0x3e + b"NATIVE " + tag.encode())


def dll_tag(path):
    """'builtin' or the tag a fake native dll was written with."""
    with open(path, "rb") as f:
        f.seek(0x40)
        data = f.read(64)
    if data.startswith(BUILTIN_MARKER):
        return "builtin"
    assert data.startswith(b"NATIVE "), data
    return data[7:].split(b"\0")[0].decode()


GFX_DLLS = ["d3d8", "d3d9", "d3d10", "d3d10_1", "d3d10core", "d3d11", "d3d12", "d3d12core", "dxgi"]


def make_dist(base):
    """A minimal Proton dist tree with fake PE files and default prefixes."""
    files = os.path.join(base, "files")
    write(os.path.join(base, "version"), b"1 mproton-test\n")
    write(os.path.join(files, "share/wine/wine.inf"), b";inf\n")
    os.makedirs(os.path.join(files, "share/fonts"), exist_ok=True)
    os.makedirs(os.path.join(files, "share/wine/fonts"), exist_ok=True)
    write(os.path.join(files, "share/fonts/liberation.ttf"))
    os.makedirs(os.path.join(files, "bin"), exist_ok=True)
    os.makedirs(os.path.join(files, "bin-arm64"), exist_ok=True)
    for pfx in ("default_pfx", "default_pfx_arm64"):
        root = os.path.join(files, "share", pfx)
        write(os.path.join(root, "system.reg"),
              b'WINE REGISTRY Version 2\n\n[Software\\\\Microsoft\\\\Cryptography] 1\n"MachineGuid"="old"\n')
        write(os.path.join(root, "user.reg"), b"WINE REGISTRY Version 2\n")
        os.makedirs(os.path.join(root, "dosdevices"), exist_ok=True)
        for sysdir in ("system32", "syswow64"):
            for dll in GFX_DLLS + ["kernel32", "ntdll"]:
                builtin_dll(os.path.join(root, "drive_c/windows", sysdir, dll + ".dll"))
    for comp, dlls in (("dxvk", ["d3d8", "d3d9", "d3d10core", "d3d11", "dxgi"]),
                       ("vkd3d-proton", ["d3d12", "d3d12core"]),
                       ("dxmt", ["d3d10core", "d3d11", "dxgi", "winemetal"])):
        for arch in PE_ARCHES:
            for dll in dlls:
                native_dll(os.path.join(files, "lib/wine", comp, arch, dll + ".dll"), comp)
    # OpenVR/OpenXR bits upstream copies unconditionally (Linux hosts only in mproton)
    for arch in PE_ARCHES:
        native_dll(os.path.join(files, "lib/wine", arch, "vrclient.dll"), "vr")
        native_dll(os.path.join(files, "lib/wine", arch, "vrclient_x64.dll"), "vr")
        native_dll(os.path.join(files, "lib/wine/dxvk", arch, "openvr_api_dxvk.dll"), "vr")
    for arch in PE_ARCHES:
        for dll in ("nvapi64", "nvofapi64", "nvapi"):
            native_dll(os.path.join(files, "lib/wine/nvapi", arch, dll + ".dll"), "nvapi")
    write(os.path.join(files, "share/openxr/wineopenxr64.json"), b"{}")
    return base


def make_gptk(base, pe_arch="aarch64-windows", unix_arch="aarch64-unix"):
    lib = os.path.join(base, "redist", "lib")
    os.makedirs(os.path.join(lib, "external", "D3DMetal.framework"), exist_ok=True)
    for dll in ("d3d12", "d3d11", "d3d10", "dxgi"):
        native_dll(os.path.join(lib, "wine", pe_arch, dll + ".dll"), "gptk")
        write(os.path.join(lib, "wine", unix_arch, dll + ".so"))
    return base


def make_mach9_bundle(root, dxgi=("dxmt", "wined3d"), d3d9=("mtld3d", "wined3d"), steam=True):
    """A fake mach9 dist/wine: bin/wine -> wine.app loader, compatdb.so, trees.

    The layout follows mach9's docs/building.md "Bundle layout".
    """
    unix = os.path.join(root, "lib/wine/aarch64-unix")
    write(os.path.join(unix, "compatdb.so"))
    write(os.path.join(unix, "wine.app/Contents/MacOS/wine"))
    os.makedirs(os.path.join(root, "bin"), exist_ok=True)
    os.symlink("../lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine", os.path.join(root, "bin/wine"))
    write(os.path.join(root, "bin/wineserver"))
    write(os.path.join(root, "share/wine/wine.inf"), b";inf\n")
    write(os.path.join(root, "share/wine/fonts/tahoma.ttf"))
    write(os.path.join(root, "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"), b"{}")
    for arch in ("aarch64-windows", "i386-windows"):
        for dll in GFX_DLLS + ["kernel32", "ntdll"]:
            builtin_dll(os.path.join(root, "lib/wine", arch, dll + ".dll"))  # markers
        if steam:
            builtin_dll(os.path.join(root, "lib/wine", arch, "steam.exe"))
            builtin_dll(os.path.join(root, "lib/wine", arch, "lsteamclient.dll"))
    for family, impls in (("dxgi", dxgi), ("d3d9", d3d9)):
        for impl in impls:
            for arch in ("aarch64-windows", "i386-windows"):
                native_dll(os.path.join(root, "lib/wine", family, impl, arch, family + ".dll"), impl)
    return root


HOSTS = {
    "darwin-arm64": ("Darwin", "arm64"),
    "linux-x86_64": ("Linux", "x86_64"),
    "linux-aarch64": ("Linux", "aarch64"),
}


class LauncherEnv:
    """Sets up globals the way proton's __main__ does, against a fake tree."""

    def __init__(self, mp, tmp_path, monkeypatch):
        self.mp = mp
        self.tmp = tmp_path
        self.monkeypatch = monkeypatch
        self.dist = make_dist(str(tmp_path / "dist"))
        self.compat = str(tmp_path / "compatdata" / "12345")
        os.makedirs(self.compat)
        self.steam = str(tmp_path / "steam")
        os.makedirs(self.steam)
        self.home = str(tmp_path / "home")
        os.makedirs(self.home)
        for var in list(os.environ):
            if var.startswith(("PROTON_", "WINE", "STEAM_", "Steam", "DXVK", "VKD3D", "FEX_", "MPROTON_",
                               "LD_LIBRARY_PATH", "DYLD_", "ORIG_", "VK_")):
                monkeypatch.delenv(var, raising=False)
        monkeypatch.setenv("HOME", self.home)
        monkeypatch.setenv("STEAM_COMPAT_DATA_PATH", self.compat)
        monkeypatch.setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH", self.steam)
        monkeypatch.setattr(os, "sync", lambda: None)

    def set_host(self, system, machine):
        self.monkeypatch.setattr("platform.system", lambda: system)
        self.monkeypatch.setattr("platform.machine", lambda: machine)
        self.mp.g_host = mproton_host.HostPlatform.detect()
        return self.mp.g_host

    def start(self, update_prefix_files=True, **env):
        for k, v in env.items():
            self.monkeypatch.setenv(k, v)
        mp = self.mp
        mp.g_proton = mp.Proton(self.dist)
        mp.g_compatdata = mp.CompatData(self.compat)
        mp.g_session = mp.Session()
        mp.g_session.init_wine()
        mp.g_session.init_session(update_prefix_files)
        return mp.g_session

    @property
    def pfx(self):
        return os.path.join(self.compat, "pfx")

    def sys32(self, dll, wow64=False):
        return os.path.join(self.pfx, "drive_c/windows", "syswow64" if wow64 else "system32", dll + ".dll")


@pytest.fixture
def mp():
    return load_launcher()


@pytest.fixture(params=sorted(HOSTS))
def any_host(request):
    return HOSTS[request.param]


@pytest.fixture
def launcher(mp, tmp_path, monkeypatch):
    return LauncherEnv(mp, tmp_path, monkeypatch)


@pytest.fixture
def darwin(launcher):
    launcher.set_host("Darwin", "arm64")
    return launcher


class Mach9Env(LauncherEnv):
    """A Darwin launch against a fake mach9 bundle; Wine is never executed.

    ``procs`` records every run_proc argv.  ``wineboot -i`` is simulated by
    creating system32/syswow64 with placeholders for the bundle's builtins
    (unless ``wineboot_rc`` is non-zero).
    """

    def __init__(self, mp, tmp_path, monkeypatch, **bundle_kw):
        super().__init__(mp, tmp_path, monkeypatch)
        self.bundle = make_mach9_bundle(str(tmp_path / "mach9" / "wine"), **bundle_kw)
        self.procs = []
        self.envs = []
        self.wineboot_rc = 0
        self.wineboot_placeholders = True
        monkeypatch.setenv("MPROTON_WINE_DIR", self.bundle)
        self.set_host("Darwin", "arm64")
        env = self

        def run_proc(session, args, local_env=None):
            env.procs.append(list(args))
            env.envs.append(dict(local_env if local_env is not None else session.env))
            if args[1:3] == ["wineboot", "-i"]:
                if env.wineboot_rc:
                    return env.wineboot_rc
                pfx = (local_env or session.env)["WINEPREFIX"]
                for sysdir, arch in (("system32", "aarch64-windows"), ("syswow64", "i386-windows")):
                    d = os.path.join(pfx, "drive_c/windows", sysdir)
                    os.makedirs(d, exist_ok=True)
                    if not env.wineboot_placeholders:
                        continue
                    for f in os.listdir(os.path.join(env.bundle, "lib/wine", arch)):
                        builtin_dll(os.path.join(d, f))
                os.makedirs(os.path.join(pfx, "dosdevices"), exist_ok=True)
            return 0
        monkeypatch.setattr(mp.Session, "run_proc", run_proc)

    def rules(self):
        with open(os.path.join(self.compat, "compatdb.rules")) as f:
            return f.read()

    def rule_lines(self):
        return self.rules().split("\n")


@pytest.fixture
def mach9(mp, tmp_path, monkeypatch):
    return Mach9Env(mp, tmp_path, monkeypatch)


@pytest.fixture
def mach9_factory(mp, tmp_path, monkeypatch):
    return lambda **kw: Mach9Env(mp, tmp_path, monkeypatch, **kw)


@pytest.fixture
def linux(launcher):
    launcher.set_host("Linux", "x86_64")
    return launcher
