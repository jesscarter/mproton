import os

import pytest

from conftest import dll_tag, make_gptk


def read(path):
    with open(path) as f:
        return f.read()


def test_prefix_created(darwin):
    darwin.start()
    pfx = darwin.pfx
    assert os.readlink(os.path.join(pfx, "dosdevices/c:")) == "../drive_c"
    assert os.readlink(os.path.join(pfx, "dosdevices/z:")) == "/"
    assert os.path.exists(os.path.join(pfx, "creation_sync_guard"))
    assert read(os.path.join(darwin.compat, "version")).strip() == darwin.mp.CURRENT_PREFIX_VERSION
    assert "MachineGuid" in read(os.path.join(pfx, "system.reg"))
    assert '"old"' not in read(os.path.join(pfx, "system.reg"))  # fresh GUID
    tracked = read(os.path.join(darwin.compat, "tracked_files")).split()
    assert "drive_c/windows/system32/kernel32.dll" in tracked
    # copied from the arm64 default prefix
    assert os.path.exists(os.path.join(pfx, ".update-timestamp"))
    assert os.path.islink(os.path.join(pfx, "drive_c/windows/Fonts/liberation.ttf"))


def test_prefix_drives(darwin, tmp_path):
    lib = tmp_path / "lib" / "steamapps"
    (lib / "common" / "Game").mkdir(parents=True)
    darwin.start(STEAM_COMPAT_INSTALL_PATH=str(lib / "common" / "Game"),
                 STEAM_COMPAT_LIBRARY_PATHS=str(lib), PROTON_SET_STEAM_DRIVE="1")
    assert os.readlink(os.path.join(darwin.pfx, "dosdevices/s:")) == str(tmp_path / "lib")
    assert os.readlink(os.path.join(darwin.pfx, "dosdevices/t:")) == darwin.steam


def test_darwin_skips_casefold_ioctl(darwin, monkeypatch):
    def boom(*a, **kw):
        raise AssertionError("ext4 ioctl called on macOS")
    monkeypatch.setattr("fcntl.ioctl", boom)
    darwin.start()


def test_case_sensitive_volume_warning(darwin, capsys):
    darwin.start()
    assert "case-sensitive volume" in capsys.readouterr().err


def test_config_info_records_backend(darwin):
    darwin.start(PROTON_USE_DXMT="1")
    info = read(os.path.join(darwin.compat, "config_info")).splitlines()
    assert "dxmt" in info


def test_steam_files_copied_when_present(darwin):
    from conftest import native_dll
    native_dll(os.path.join(darwin.steam, "legacycompat", "steamclient64.dll"), "steam")
    darwin.start()
    dst = os.path.join(darwin.pfx, "drive_c/Program Files (x86)/Steam/steamclient64.dll")
    assert dll_tag(dst) == "steam"


def test_missing_legacycompat_is_tolerated(darwin):
    darwin.start()  # macOS Steam may have no legacycompat/ dir at all


@pytest.mark.parametrize("host", [("Darwin", "arm64"), ("Linux", "x86_64"), ("Linux", "aarch64")])
def test_dxvk_placement(launcher, host):
    launcher.set_host(*host)
    launcher.start()
    for dll in ("d3d11", "d3d10core", "d3d9", "dxgi"):
        assert dll_tag(launcher.sys32(dll)) == "dxvk"
        assert dll_tag(launcher.sys32(dll, wow64=True)) == "dxvk"
    assert dll_tag(launcher.sys32("d3d12")) == "vkd3d-proton"
    assert dll_tag(launcher.sys32("d3d8")) == "builtin"
    assert not os.path.exists(launcher.sys32("winemetal"))


def test_wined3d_placement(darwin):
    s = darwin.start(PROTON_USE_WINED3D="1")
    for dll in ("d3d11", "d3d12", "d3d9", "dxgi", "d3d10core"):
        assert dll_tag(darwin.sys32(dll)) == "builtin", dll
    assert "d3d11" not in s.dlloverrides


def test_dxmt_placement(darwin):
    s = darwin.start(PROTON_USE_DXMT="1")
    assert s.graphics_backend == "dxmt"
    for dll in ("d3d11", "d3d10core", "dxgi", "winemetal"):
        assert dll_tag(darwin.sys32(dll)) == "dxmt", dll
        assert s.dlloverrides[dll] == "n"
    assert dll_tag(darwin.sys32("d3d9")) == "dxvk"
    assert dll_tag(darwin.sys32("d3d12")) == "vkd3d-proton"


def test_dxmt_via_steam_compat_config(darwin):
    s = darwin.start(STEAM_COMPAT_CONFIG="dxmt")
    assert s.graphics_backend == "dxmt"


def test_switching_away_from_dxmt_cleans_up(darwin):
    darwin.start(PROTON_USE_DXMT="1")
    assert os.path.exists(darwin.sys32("winemetal"))
    darwin.monkeypatch.delenv("PROTON_USE_DXMT")
    darwin.start()
    assert not os.path.exists(darwin.sys32("winemetal"))
    assert dll_tag(darwin.sys32("d3d11")) == "dxvk"


def test_d3dmetal_without_gptk_falls_back(darwin, capsys):
    s = darwin.start(PROTON_USE_D3DMETAL="1")
    assert s.graphics_backend == "dxvk"
    assert dll_tag(darwin.sys32("d3d11")) == "dxvk"
    assert "D3DMetal requested but unavailable" in capsys.readouterr().err


def test_d3dmetal_with_matching_gptk(darwin, tmp_path):
    gptk = make_gptk(str(tmp_path / "gptk"))
    s = darwin.start(PROTON_USE_D3DMETAL="1", PROTON_D3DMETAL_PATH=gptk)
    assert s.graphics_backend == "d3dmetal"
    wine_dir = os.path.join(gptk, "redist/lib/wine")
    assert s.env["WINEDLLPATH"].split(":")[0] == wine_dir
    for dll in ("d3d12", "d3d11", "d3d10", "dxgi"):
        assert dll_tag(darwin.sys32(dll)) == "builtin"  # nothing from GPTK is copied
        assert s.dlloverrides[dll] == "b"
    assert not os.path.exists(os.path.join(darwin.pfx, "drive_c/windows/system32/D3DMetal.framework"))


def test_d3dmetal_x86_only_gptk_rejected_on_arm64(darwin, tmp_path):
    gptk = make_gptk(str(tmp_path / "gptk"), "x86_64-windows", "x86_64-unix")
    s = darwin.start(PROTON_USE_D3DMETAL="1", PROTON_D3DMETAL_PATH=gptk)
    assert s.graphics_backend == "dxvk"
    assert gptk not in s.env["WINEDLLPATH"]


def test_destroyprefix_tracked_files(darwin):
    darwin.start()
    darwin.mp.g_compatdata.remove_tracked_files()
    assert not os.path.exists(darwin.sys32("kernel32"))
    assert not os.path.exists(os.path.join(darwin.compat, "version"))


def test_nvapi_only_on_linux_x86_64(launcher):
    launcher.set_host("Linux", "x86_64")
    launcher.start()
    assert dll_tag(launcher.sys32("nvapi64")) == "nvapi"


def test_no_nvapi_on_darwin_rosetta(launcher):
    launcher.set_host("Darwin", "x86_64")
    s = launcher.start()
    assert not os.path.exists(launcher.sys32("nvapi64"))
    assert "nvapi64" not in s.dlloverrides


def test_no_vr_files_on_darwin(darwin):
    darwin.start()
    assert not os.path.exists(os.path.join(darwin.pfx, "drive_c/vrclient"))
    assert not os.path.exists(darwin.sys32("openvr_api_dxvk"))
