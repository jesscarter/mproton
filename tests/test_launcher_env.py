import json
import os


def overrides(session):
    return dict(kv.split("=", 1) for kv in session.env["WINEDLLOVERRIDES"].split(";"))


def test_darwin_arm64_layout(darwin):
    s = darwin.start()
    mp = darwin.mp
    assert mp.g_proton.host_pe_arch == "aarch64-windows"
    assert mp.g_proton.bin_dir.endswith("files/bin-arm64/")
    assert mp.g_proton.default_pfx_dir.endswith("default_pfx_arm64/")
    assert s.env["WINEPREFIX"] == darwin.pfx + "/"


def test_darwin_env_has_no_linker_vars(darwin):
    s = darwin.start()
    for var in ("LD_LIBRARY_PATH", "ORIG_LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
                "DYLD_FALLBACK_LIBRARY_PATH"):
        assert var not in s.env, var
    assert s.env["GST_PLUGIN_SYSTEM_PATH_1_0"].endswith("files/lib/aarch64-apple-darwin/gstreamer-1.0")
    assert s.env["PATH"].startswith(darwin.mp.g_proton.bin_dir)


def test_darwin_dyld_fallback_opt_in(darwin, monkeypatch):
    monkeypatch.setenv("MPROTON_DYLD_FALLBACK", "1")
    darwin.set_host("Darwin", "arm64")
    s = darwin.start()
    assert s.env["DYLD_FALLBACK_LIBRARY_PATH"].endswith("files/lib/aarch64-apple-darwin")
    assert "ubuntu12_64" not in s.env["DYLD_FALLBACK_LIBRARY_PATH"]
    assert "LD_LIBRARY_PATH" not in s.env


def test_linux_env_unchanged(linux):
    s = linux.start()
    ld = s.env["LD_LIBRARY_PATH"].split(":")
    assert ld[0].endswith("ubuntu12_64/video/")
    assert any(p.endswith("files/lib/x86_64-linux-gnu") for p in ld)
    assert s.env["WINEFSYNC"] == "1"
    assert "WINEMSYNC" not in s.env
    assert "FEX_APP_CONFIG" not in s.env


def test_darwin_uses_msync(darwin, monkeypatch):
    monkeypatch.setenv("WINEFSYNC", "1")
    monkeypatch.setenv("WINENTSYNC", "1")
    s = darwin.start()
    assert s.env["WINEMSYNC"] == "1"
    assert "WINEFSYNC" not in s.env and "WINENTSYNC" not in s.env


def test_darwin_no_msync_env(darwin):
    s = darwin.start(PROTON_NO_MSYNC="1")
    assert "WINEMSYNC" not in s.env


def test_darwin_nofsync_game_disables_msync(darwin):
    s = darwin.start(SteamAppId="2630", SteamGameId="2630")  # Call of Duty 2: nofsync in compat table
    assert "nofsync" in s.compat_config
    assert "WINEMSYNC" not in s.env


def test_darwin_fex_config(darwin):
    s = darwin.start(SteamGameId="292030", STEAM_FEX_TSOENABLED="1",
                     STEAM_COMPAT_FEX_CONFIG="TSOEnabled:0,Multiblock:1")
    path = s.env["FEX_APP_CONFIG"]
    assert path == os.path.join(darwin.compat, "proton-fex-config.json")
    with open(path) as f:
        cfg = json.load(f)
    assert cfg["Config"] == {"TSOEnabled": "0", "Multiblock": "1"}
    assert "setup*" in cfg["AppOverrides"]  # Witcher 3 profile from the upstream table
    assert s.env["FEX_APP_CONFIG_LOCATION"].endswith("files/share/fex-emu/")


def test_user_fex_config_is_respected(darwin):
    s = darwin.start(FEX_APP_CONFIG="/custom.json")
    assert s.env["FEX_APP_CONFIG"] == "/custom.json"
    assert not os.path.exists(os.path.join(darwin.compat, "proton-fex-config.json"))


def test_darwin_nvapi_disabled_without_proc(darwin):
    s = darwin.start(SteamAppId="1808500", SteamGameId="1808500")  # ARC Raiders: /proc/modules probe upstream
    assert "disablenvapi" in s.compat_config


def test_compat_tables_still_apply(darwin):
    s = darwin.start(SteamAppId="257420", SteamGameId="19900")
    assert "hidevggpu" in s.compat_config
    assert s.env["WINE_CPU_TOPOLOGY"] == "16"


def test_default_overrides(darwin):
    o = overrides(darwin.start())
    assert o["steam.exe"] == "b"
    for dll in ("d3d11", "d3d10core", "d3d9", "dxgi", "d3d12", "d3d12core"):
        assert o[dll] == "n", dll


def test_run_on_darwin_uses_wine_without_adverb(darwin, monkeypatch):
    s = darwin.start()
    monkeypatch.setattr("shutil.which", lambda name: "/usr/bin/" + name)
    calls = []
    monkeypatch.setattr(s, "run_proc", lambda args, env=None: calls.append(args) or 0)
    monkeypatch.setattr("sys.argv", ["mproton", "waitforexitandrun", "game.exe"])
    assert s.run() == 0
    argv = calls[0]
    assert argv[0] == darwin.mp.g_proton.wine_bin
    assert argv[0].endswith("files/bin-arm64/wine")
    assert argv[1:] == ["c:\\windows\\system32\\steam.exe", "game.exe"]
    assert "WINELOADERNOEXEC" not in s.env


def test_run_on_darwin_x86_64_has_no_preloader(launcher, monkeypatch):
    launcher.set_host("Darwin", "x86_64")  # Rosetta bootstrap path
    s = launcher.start()
    calls = []
    monkeypatch.setattr(s, "run_proc", lambda args, env=None: calls.append(args) or 0)
    monkeypatch.setattr("sys.argv", ["mproton", "run", "game.exe"])
    s.run()
    assert "wine-preloader" not in calls[0][0]
    assert s.env.get("WINEFSYNC") is None


def test_run_on_linux_uses_runtime_adverb(linux, monkeypatch):
    s = linux.start()
    monkeypatch.setattr("shutil.which", lambda name: "/usr/bin/" + name)
    calls = []
    monkeypatch.setattr(s, "run_proc", lambda args, env=None: calls.append(args) or 0)
    monkeypatch.setattr("sys.argv", ["proton", "run", "game.exe"])
    s.run()
    assert calls[0][:2] == ["steam-runtime-launcher-interface-0", "proton"]
    assert calls[0][2].endswith("wine-preloader")
