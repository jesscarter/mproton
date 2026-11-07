"""The launcher on a mocked mach9 bundle: runtime lookup, env, prefix, rules, argv."""

import os
import sys

import pytest

import mproton_runtime as rt
from conftest import builtin_dll, dll_tag, make_mach9_bundle, native_dll


def read(path):
    with open(path) as f:
        return f.read()


# ---- locating the runtime -------------------------------------------------

def test_find_bundle_explicit(tmp_path):
    root = make_mach9_bundle(str(tmp_path / "dist" / "wine"))
    for value in (root, str(tmp_path / "dist")):  # the bundle, or its parent
        b, reason = rt.find_bundle("/nowhere", {"MPROTON_WINE_DIR": value})
        assert reason is None and b.root == root + "/"


def test_find_bundle_next_to_launcher(tmp_path):
    base = tmp_path / "compatibilitytools.d" / "mproton"
    root = make_mach9_bundle(str(base / "wine"))
    b, reason = rt.find_bundle(str(base), {})
    assert (b.root, reason) == (root + "/", None)


def test_find_bundle_in_place_of_files(tmp_path):
    root = make_mach9_bundle(str(tmp_path / "files"))
    b, _ = rt.find_bundle(str(tmp_path), {})
    assert b.root == root + "/"


def test_find_bundle_rejects_proton_layout(tmp_path):
    os.makedirs(tmp_path / "files" / "bin")
    open(tmp_path / "files" / "bin" / "wine", "w").close()
    assert rt.find_bundle(str(tmp_path), {}) == (None, None)
    b, reason = rt.find_bundle(str(tmp_path), {"MPROTON_WINE_DIR": str(tmp_path / "files")})
    assert b is None and "not a mach9 bundle" in reason


def test_bundle_paths(mach9):
    mach9.start()
    p = mach9.mp.g_proton
    assert p.mach9.root == mach9.bundle + "/"
    assert p.wine_bin == mach9.bundle + "/bin/wine"
    assert os.path.realpath(p.wine_bin).endswith("wine.app/Contents/MacOS/wine")
    assert p.wineserver_bin == mach9.bundle + "/bin/wineserver"
    assert p.host_pe_arch == "aarch64-windows" and p.wow64_pe_arch == "i386-windows"
    assert p.default_pfx_dir is None and not p.missing_default_prefix()


def test_bad_explicit_bundle_exits(launcher, monkeypatch, tmp_path):
    launcher.set_host("Darwin", "arm64")
    monkeypatch.setenv("MPROTON_WINE_DIR", str(tmp_path / "missing"))
    with pytest.raises(SystemExit):
        launcher.mp.Proton(launcher.dist)


def test_darwin_without_bundle_keeps_proton_layout(darwin):
    darwin.start()
    p = darwin.mp.g_proton
    assert p.mach9 is None and p.bin_dir.endswith("files/bin-arm64/")


def test_linux_ignores_bundle(linux, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("MPROTON_WINE_DIR", make_mach9_bundle(str(tmp_path / "wine")))
    s = linux.start()
    assert linux.mp.g_proton.mach9 is None
    assert linux.mp.g_proton.wine_bin.endswith("files/bin/wine")
    assert "WINE_COMPATDB_FILE" not in s.env
    assert "steam.exe=b" in s.env["WINEDLLOVERRIDES"]
    assert "ignored on Linux" in capsys.readouterr().err


# ---- environment ----------------------------------------------------------

def test_env_construction(mach9):
    s = mach9.start()
    env = s.env
    assert env["WINEPREFIX"] == mach9.pfx + "/"
    assert env["PATH"].startswith(mach9.bundle + "/bin/")
    assert env["WINE_COMPATDB_FILE"] == os.path.join(mach9.compat, "compatdb.rules")
    assert env["WINEMSYNC"] == "1"
    for var in ("WINEDLLOVERRIDES", "WINE_LARGE_ADDRESS_AWARE", "WINEDLLPATH", "LD_LIBRARY_PATH",
                "DYLD_FALLBACK_LIBRARY_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0", "WINE_GST_REGISTRY_DIR",
                "MEDIACONV_BLANK_VIDEO_FILE", "ESPEAK_DATA_PATH", "VK_DRIVER_FILES", "VK_ICD_FILENAMES",
                "WINE_PROF_SERVICE", "FEX_HARDWARE_TSO", "WINENCPU", "MPROTON_D3DMETAL"):
        assert var not in env, var


def test_env_passthrough(mach9):
    s = mach9.start(FEX_HARDWARE_TSO="0", WINENCPU="8", WINEMSYNC="1",
                    VK_DRIVER_FILES="/opt/icd.json", WINE_COMPATDB="exe=Game.exe;d3d9=wined3d",
                    WINEDLLPATH="/extra", FEX_HOSTFEATURES="enableafp")
    for var, value in (("FEX_HARDWARE_TSO", "0"), ("WINENCPU", "8"), ("WINEMSYNC", "1"),
                       ("VK_DRIVER_FILES", "/opt/icd.json"), ("WINE_COMPATDB", "exe=Game.exe;d3d9=wined3d"),
                       ("WINEDLLPATH", "/extra"), ("FEX_HOSTFEATURES", "enableafp")):
        assert s.env[var] == value, var


@pytest.mark.parametrize("env", [{"PROTON_NO_MSYNC": "1"}, {"WINEMSYNC": "1", "PROTON_NO_MSYNC": "1"}])
def test_msync_off_is_zero(mach9, env):
    # mach9: msync is on when WINEMSYNC is unset, so off has to be "0"
    assert mach9.start(**env).env["WINEMSYNC"] == "0"


def test_cpu_topology_becomes_winencpu(mach9):
    assert mach9.start(PROTON_CPU_TOPOLOGY="4:0,1,2,3").env["WINENCPU"] == "4"


def test_default_cpu_limit_becomes_winencpu(mach9):
    assert mach9.start(SteamGameId="19900", SteamAppId="19900").env["WINENCPU"] == "16"  # Far Cry 2


def test_inherited_conflicts_dropped(mach9):
    s = mach9.start(WINE_PROF_SERVICE="inherited", WINE_COMPATDB_FILE="/tmp/other",
                    WINE_LARGE_ADDRESS_AWARE="1")
    assert "WINE_PROF_SERVICE" not in s.env
    assert s.env["WINE_COMPATDB_FILE"] == os.path.join(mach9.compat, "compatdb.rules")
    assert "WINE_LARGE_ADDRESS_AWARE" not in s.env


def test_profiler_on_purpose(mach9):
    assert mach9.start(MPROTON_PROF_SERVICE="com.example.prof").env["WINE_PROF_SERVICE"] == "com.example.prof"


# ---- rules ----------------------------------------------------------------

def test_default_rules_file(mach9):
    mach9.start()
    assert mach9.rules() == (
        "exe=*;large_address_aware=true;"
        "dll_overrides=steam.exe=b;dll_overrides=dotnetfx35.exe=b;dll_overrides=dotnetfx35setup.exe=b;"
        "dll_overrides=beclient.dll=b,n;dll_overrides=beclient_x64.dll=b,n;dll_overrides=winebth.sys=d;"
        "dll_overrides=opencl=n,d")


@pytest.mark.parametrize("env,expect", [
    ({}, ""),
    ({"PROTON_USE_DXMT": "1"}, "dxgi=dxmt;"),
    ({"PROTON_USE_WINED3D": "1"}, "dxgi=wined3d;d3d9=wined3d;"),
    ({"PROTON_USE_WINED3D11": "1"}, "dxgi=wined3d;d3d9=wined3d;"),
    ({"PROTON_USE_MTLD3D": "1"}, "d3d9=mtld3d;"),
    ({"PROTON_USE_DXVK": "1"}, ""),  # no dxvk tree in this bundle
    ({"PROTON_USE_D3DMETAL": "1"}, ""),  # no d3dmetal tree either
    ({"STEAM_COMPAT_CONFIG": "wined3d"}, "dxgi=wined3d;d3d9=wined3d;"),
    ({"MPROTON_DXGI": "wined3d", "MPROTON_D3D9": "wined3d"}, "dxgi=wined3d;d3d9=wined3d;"),
])
def test_env_vars_to_rules(mach9, env, expect):
    s = mach9.start(**env)
    assert mach9.rule_lines()[0].startswith("exe=*;" + expect + "large_address_aware=true;")


def test_dxvk_and_d3dmetal_trees(mach9_factory):
    env = mach9_factory(dxgi=("dxmt", "wined3d", "dxvk", "d3dmetal"), d3d9=("mtld3d", "wined3d", "dxvk"))
    s = env.start(PROTON_USE_DXVK="1")
    assert s.graphics_backend == "dxvk"
    assert env.rule_lines()[0].startswith("exe=*;dxgi=dxvk;d3d9=dxvk;")


def test_d3dmetal_tree(mach9_factory):
    env = mach9_factory(dxgi=("dxmt", "wined3d", "d3dmetal"))
    s = env.start(PROTON_USE_D3DMETAL="1")
    assert s.graphics_backend == "d3dmetal"
    assert env.rule_lines()[0].startswith("exe=*;dxgi=d3dmetal;")
    assert "WINEDLLPATH" not in s.env  # no GPTK search path: the tree is the mechanism


def test_d3dmetal_missing_falls_back(mach9, capsys):
    s = mach9.start(PROTON_USE_D3DMETAL="1", PROTON_D3DMETAL_PATH="/nonexistent")
    assert s.graphics_backend == "mach9"
    assert "D3DMetal requested but unavailable" in capsys.readouterr().err


def test_no_d3d12(mach9):
    mach9.start(PROTON_NO_D3D12="1")
    assert "dll_overrides=d3d12=;dll_overrides=d3d12core=" in mach9.rules()


def test_user_dlloverrides_become_rules(mach9):
    s = mach9.start(WINEDLLOVERRIDES="dinput8=n,b")
    assert "WINEDLLOVERRIDES" not in s.env
    assert mach9.rules().startswith("exe=*;large_address_aware=true;dll_overrides=dinput8=n,b;dll_overrides=steam.exe=b")


def test_game_specific_overrides_reach_rules(mach9):
    mach9.start(SteamGameId="500810", SteamAppId="500810")  # Arcanum: ddraw=n,b
    assert "dll_overrides=ddraw=n,b" in mach9.rules()


def test_steam_app_rule_for_explicit_backend(mach9):
    mach9.start(PROTON_USE_WINED3D="1", SteamAppId="12210", SteamGameId="12210")
    assert mach9.rule_lines()[1] == "exe=steam:12210;dxgi=wined3d;d3d9=wined3d"


def test_rules_rewritten_each_launch(mach9):
    mach9.start(PROTON_USE_WINED3D="1")
    assert "dxgi=wined3d" in mach9.rules()
    mach9.monkeypatch.delenv("PROTON_USE_WINED3D")
    mach9.start()
    assert "dxgi=" not in mach9.rules()


def test_runinprefix_writes_rules(mach9):
    mach9.start(update_prefix_files=False, PROTON_USE_DXMT="1")
    assert "dxgi=dxmt" in mach9.rules()
    assert mach9.procs == []  # no wineboot without prefix setup


def test_log_file_records_rules(mach9, tmp_path):
    s = mach9.start(PROTON_LOG="1", PROTON_LOG_DIR=str(tmp_path / "logs"), SteamGameId="1")
    log = read(str(tmp_path / "logs" / "steam-1.log"))
    assert "Wine: mach9 bundle " + mach9.bundle in log
    assert "compatdb rules (" in log and "exe=*;" in log


# ---- prefix creation ------------------------------------------------------

def test_prefix_created_with_wineboot(mach9):
    mach9.start()
    wine = mach9.bundle + "/bin/wine"
    assert mach9.procs[:2] == [[wine, "wineboot", "-i"], [mach9.bundle + "/bin/wineserver", "-w"]]
    boot_env = mach9.envs[0]
    assert boot_env["WINEPREFIX"] == mach9.pfx + "/"
    assert boot_env["WINEDEBUG"] == "-all"
    assert "WINE_COMPATDB_FILE" not in boot_env  # rules are written later
    pfx = mach9.pfx
    assert os.path.exists(os.path.join(pfx, "creation_sync_guard"))
    assert read(os.path.join(mach9.compat, "version")).strip() == mach9.mp.MACH9_PREFIX_VERSION
    info = read(os.path.join(mach9.compat, "config_info")).splitlines()
    assert info[:3] == [mach9.mp.MACH9_PREFIX_VERSION, mach9.bundle + "/", "mach9"]
    assert os.readlink(os.path.join(pfx, "dosdevices/c:")) == "../drive_c"
    assert os.readlink(os.path.join(pfx, "dosdevices/z:")) == "/"
    assert os.path.islink(os.path.join(pfx, "drive_c/windows/Fonts/tahoma.ttf"))


def test_no_dll_copies_or_default_prefix(mach9):
    mach9.start(PROTON_USE_DXMT="1")
    sys32 = os.path.join(mach9.pfx, "drive_c/windows/system32")
    # only wineboot's placeholders: nothing native was copied in for a backend
    for dll in ("d3d11", "dxgi", "d3d12", "d3d9", "winemetal"):
        path = os.path.join(sys32, dll + ".dll")
        assert not os.path.exists(path) or dll_tag(path) == "builtin", dll
    assert not os.path.exists(os.path.join(sys32, "winemetal.dll"))
    assert not os.path.exists(os.path.join(mach9.pfx, ".update-timestamp"))  # Wine updates the prefix itself


def test_wineboot_runs_once(mach9):
    mach9.start()
    mach9.start()
    assert sum(1 for p in mach9.procs if p[1:] == ["wineboot", "-i"]) == 1


def test_wineboot_failure_is_retried(mach9, capsys):
    mach9.wineboot_rc = 3
    mach9.start()
    assert not os.path.exists(os.path.join(mach9.pfx, "creation_sync_guard"))
    assert "wineboot -i failed (3)" in capsys.readouterr().err
    mach9.wineboot_rc = 0
    mach9.start()
    assert os.path.exists(os.path.join(mach9.pfx, "creation_sync_guard"))


def test_steam_exe_placed_from_bundle(mach9):
    mach9.wineboot_placeholders = False  # a prefix made before the bundle had steam.exe
    mach9.start()
    for sysdir in ("system32", "syswow64"):
        for f in ("steam.exe", "lsteamclient.dll"):
            path = os.path.join(mach9.pfx, "drive_c/windows", sysdir, f)
            assert dll_tag(path) == "builtin", path
    tracked = read(os.path.join(mach9.compat, "tracked_files")).split("\n")
    assert "drive_c/windows/system32/steam.exe" in tracked


def test_steam_exe_placeholder_kept(mach9):
    mach9.start()
    tracked = read(os.path.join(mach9.compat, "tracked_files")).split("\n")
    assert "drive_c/windows/system32/steam.exe" not in tracked  # wineboot's own, not ours


def test_legacycompat_steam_files(mach9):
    native_dll(os.path.join(mach9.steam, "legacycompat", "steamclient64.dll"), "steam")
    mach9.start()
    dst = os.path.join(mach9.pfx, "drive_c/Program Files (x86)/Steam/steamclient64.dll")
    assert dll_tag(dst) == "steam"


def test_proton_prefix_warns(mach9, capsys):
    os.makedirs(mach9.pfx)
    with open(os.path.join(mach9.compat, "version"), "w") as f:
        f.write("10.0-200\n")
    mach9.start()
    assert "was made for Proton 10.0-200" in capsys.readouterr().err


# ---- running --------------------------------------------------------------

def test_run_argv_with_steam_exe(mach9, monkeypatch):
    mach9.start()
    monkeypatch.setattr(sys, "argv", ["mproton", "waitforexitandrun", "/games/Game.exe", "-w"])
    mach9.procs.clear()
    mach9.mp.g_session.run()
    assert mach9.procs == [[mach9.bundle + "/bin/wine", "c:\\windows\\system32\\steam.exe", "/games/Game.exe", "-w"]]


def test_run_argv_without_steam_exe(mach9_factory, monkeypatch, capsys):
    env = mach9_factory(steam=False)
    env.start()
    monkeypatch.setattr(sys, "argv", ["mproton", "run", "/games/Game.exe"])
    env.procs.clear()
    env.mp.g_session.run()
    assert env.procs == [[env.bundle + "/bin/wine", "/games/Game.exe"]]
    assert "starting the game without it" in capsys.readouterr().err


def test_run_argv_forced_direct(mach9, monkeypatch):
    mach9.start(MPROTON_NO_STEAM_EXE="1")
    monkeypatch.setattr(sys, "argv", ["mproton", "run", "/games/Game.exe"])
    mach9.procs.clear()
    mach9.mp.g_session.run()
    assert mach9.procs == [[mach9.bundle + "/bin/wine", "/games/Game.exe"]]
