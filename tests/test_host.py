import builtins

import pytest

from mproton_host import HostPlatform, normalize_machine


@pytest.mark.parametrize("raw,norm", [("arm64", "aarch64"), ("aarch64", "aarch64"),
                                      ("x86_64", "x86_64"), ("AMD64", "x86_64"), ("i686", "i386")])
def test_normalize_machine(raw, norm):
    assert normalize_machine(raw) == norm


def test_detect_uses_platform_module(monkeypatch):
    monkeypatch.setattr("platform.system", lambda: "Darwin")
    monkeypatch.setattr("platform.machine", lambda: "arm64")
    host = HostPlatform.detect()
    assert host.is_darwin and not host.is_linux
    assert host.is_arm64  # macOS reports arm64, not aarch64
    assert host.raw_machine == "arm64"


def test_darwin_has_no_runtime_or_preloader():
    host = HostPlatform("Darwin", "arm64", {})
    assert not host.uses_pressure_vessel
    assert not host.uses_wine_preloader
    assert not host.needs_casefold_ioctl
    assert not host.has_nvidia_wine_dlls


def test_linux_keeps_upstream_behaviour():
    host = HostPlatform("Linux", "x86_64", {})
    assert host.uses_pressure_vessel and host.uses_wine_preloader
    assert host.library_path_var == "LD_LIBRARY_PATH"
    assert host.host_lib_dirs("/d/lib/") == ["/d/lib/x86_64-linux-gnu", "/d/lib/aarch64-linux-gnu",
                                            "/d/lib/i386-linux-gnu"]


def test_darwin_library_path_is_rpath_by_default():
    assert HostPlatform("Darwin", "arm64", {}).library_path_var is None
    assert HostPlatform("Darwin", "arm64", {"MPROTON_DYLD_FALLBACK": "0"}).library_path_var is None


def test_darwin_dyld_fallback_opt_in():
    host = HostPlatform("Darwin", "arm64", {"MPROTON_DYLD_FALLBACK": "1"})
    assert host.library_path_var == "DYLD_FALLBACK_LIBRARY_PATH"
    assert host.host_lib_dirs("/d/lib/") == ["/d/lib/aarch64-apple-darwin"]


def test_darwin_steam_media_dirs_empty():
    assert HostPlatform("Darwin", "arm64", {}).steam_media_lib_dirs("/s") == []
    assert "/s/ubuntu12_64/video/" in HostPlatform("Linux", "x86_64", {}).steam_media_lib_dirs("/s")


@pytest.mark.parametrize("config,expect_set,expect_unset", [
    (set(), {"WINEMSYNC": "1"}, {"WINEFSYNC", "WINEESYNC", "WINENTSYNC"}),
    ({"nomsync"}, {"WINEMSYNC": "0"}, {"WINEFSYNC", "WINEESYNC", "WINENTSYNC"}),
    ({"nofsync"}, {"WINEMSYNC": "0"}, {"WINEFSYNC", "WINEESYNC", "WINENTSYNC"}),
])
def test_darwin_sync_env(config, expect_set, expect_unset):
    s, u = HostPlatform("Darwin", "arm64", {}).sync_env(config)
    assert s == expect_set
    assert set(u) == expect_unset


def test_linux_sync_env():
    host = HostPlatform("Linux", "x86_64", {})
    assert host.sync_env(set()) == ({"WINEFSYNC": "1"}, [])
    assert host.sync_env({"nofsync"}) == ({}, ["WINEFSYNC"])


def test_darwin_never_reads_proc(monkeypatch):
    real_open = builtins.open
    def guarded_open(path, *a, **kw):
        assert not str(path).startswith("/proc"), path
        return real_open(path, *a, **kw)
    monkeypatch.setattr(builtins, "open", guarded_open)
    host = HostPlatform("Darwin", "arm64", {})
    assert host.loaded_kernel_modules() is None
    assert host.max_map_count() is None


def test_linux_reads_proc_modules(monkeypatch, tmp_path):
    fake = tmp_path / "modules"
    fake.write_text("nvidia 123 0 - Live 0x0\next4 1 0 - Live 0x0\n")
    real_open = builtins.open
    monkeypatch.setattr(builtins, "open",
                        lambda p, *a, **kw: real_open(fake if p == "/proc/modules" else p, *a, **kw))
    assert HostPlatform("Linux", "x86_64", {}).loaded_kernel_modules() == {"nvidia", "ext4"}


def test_launcher_adverb(monkeypatch):
    monkeypatch.setattr("shutil.which", lambda name: "/usr/bin/" + name)
    assert HostPlatform("Darwin", "arm64", {}).launcher_adverb() == []
    assert HostPlatform("Linux", "x86_64", {}).launcher_adverb() == \
        ["steam-runtime-launcher-interface-0", "proton"]


def test_default_steam_client_path():
    host = HostPlatform("Darwin", "arm64", {"HOME": "/Users/me"})
    assert host.default_steam_client_path() == \
        "/Users/me/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS"


def test_case_probe(tmp_path):
    d = tmp_path / "drive_c"
    d.mkdir()
    # tmpfs/ext4 in CI is case-sensitive; APFS default would return True
    assert HostPlatform.dir_is_case_insensitive(str(d)) is False
    assert HostPlatform.dir_is_case_insensitive(str(tmp_path / "123")) is None


def test_clonefile_only_on_darwin():
    assert HostPlatform("Linux", "x86_64", {}).clonefile_func() is None
