"""Unit tests for mproton_library (no Steam, network or Wine needed).

Run: python3 -m unittest discover -s library/tests
"""
import importlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.steam = t / "Steam"
        self.macos = self.steam / "Steam.AppBundle/Steam/Contents/MacOS"
        self.macos.mkdir(parents=True)
        os.environ.update(MPROTON_STEAM_ROOT=str(self.steam), MPROTON_LIBRARY_HOME=str(t / "state"),
                          MPROTON_PREFIXES=str(t / "prefixes"), MPROTON_REPORTS=str(t / "reports"))
        sys.path.insert(0, str(HERE.parent))
        import mproton_library
        self.m = importlib.reload(mproton_library)
        self.m.steam_status = lambda: {"running": False, "pid": None, "started": None}
        self.m.steamcmd_info = lambda appid, refresh=False, offline=False: self.infos.get(str(appid))
        self.infos = {}

    def tearDown(self):
        self.tmp.cleanup()
        sys.path.remove(str(HERE.parent))


class DevCfg(Base):
    def cfg(self):
        return self.m.DEV_CFG

    def test_absent_roundtrip_deletes_file(self):
        st = self.m.set_dev_cfg(True)
        self.assertTrue(st["enabled"])
        self.assertEqual(self.cfg().read_text(), "@sSteamCmdForcePlatformType windows\n")
        st = self.m.set_dev_cfg(False)
        self.assertFalse(st["enabled"])
        self.assertFalse(self.cfg().exists())

    def test_existing_file_kept_byte_identical(self):
        orig = "foo 1\n@sSteamCmdForcePlatformType macos\nbar 2\n"
        self.cfg().write_text(orig)
        self.m.set_dev_cfg(True)
        self.assertEqual(self.cfg().read_text(), "foo 1\n@sSteamCmdForcePlatformType windows\nbar 2\n")
        self.m.set_dev_cfg(False)
        self.assertEqual(self.cfg().read_text(), orig)

    def test_existing_file_without_line(self):
        self.cfg().write_text("foo 1\n")
        self.m.set_dev_cfg(True)
        self.assertEqual(self.cfg().read_text(), "foo 1\n@sSteamCmdForcePlatformType windows\n")
        self.m.set_dev_cfg(False)
        self.assertEqual(self.cfg().read_text(), "foo 1\n")

    def test_on_twice_is_one_line(self):
        self.m.set_dev_cfg(True)
        self.m.set_dev_cfg(True)
        self.assertEqual(self.cfg().read_text().count("SteamCmdForcePlatformType"), 1)

    def test_backups_written(self):
        self.cfg().write_text("x\n")
        self.m.set_dev_cfg(True)
        backups = list((self.m.STATE_DIR / "steam_dev.cfg.backups").iterdir())
        self.assertEqual([b.read_text() for b in backups], ["x\n"])


class Discovery(Base):
    def test_depot_and_library(self):
        depot = self.macos / "steamapps/content/app_100/depot_101"
        (depot / "Bin").mkdir(parents=True)
        (depot / "Bin/Game.exe").write_text("MZ")
        (depot / "UnityCrashHandler64.exe").write_text("MZ")
        self.infos["100"] = {"common": {"name": "Depot Game"}, "config": {"launch": {
            "0": {"executable": "bin\\game.exe", "arguments": "-x", "config": {"oslist": "windows"}},
            "1": {"executable": "Game.app", "config": {"oslist": "macos"}}}}}
        # library: one Windows install, one Mac install
        lib = self.steam / "steamapps"
        (lib / "common/Win/").mkdir(parents=True)
        (lib / "common/Win/win.exe").write_text("MZ")
        (lib / "common/Mac/Mac.app").mkdir(parents=True)
        (lib / "appmanifest_200.acf").write_text('"AppState"\n{\n\t"appid"\t\t"200"\n\t"name"\t\t"Win Game"\n\t"installdir"\t\t"Win"\n}\n')
        (lib / "appmanifest_300.acf").write_text('"AppState"\n{\n\t"appid"\t\t"300"\n\t"name"\t\t"Mac Game"\n\t"installdir"\t\t"Mac"\n}\n')
        (lib / "libraryfolders.vdf").write_text('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' % self.steam)
        games = {g["appid"]: g for g in self.m.scan_games()}
        self.assertEqual(sorted(games), ["100", "200"])
        g = games["100"]
        self.assertEqual(g["name"], "Depot Game")
        self.assertEqual(g["launch"]["source"], "steam")
        self.assertTrue(g["launch"]["exe"].endswith("Bin/Game.exe"))
        self.assertEqual(g["launch"]["args"], "-x")
        self.assertEqual(games["200"]["launch"]["source"], "guess")  # no launch info offline
        self.assertFalse(games["200"]["verified_windows"])

    def test_override(self):
        depot = self.macos / "steamapps/content/app_100/depot_101"
        depot.mkdir(parents=True)
        (depot / "a.exe").write_text("MZ")
        (depot / "b.exe").write_text("MZ")
        s = self.m.load_settings()
        s["games"]["100"] = {"exe": "b.exe"}
        self.m.save_settings(s)
        g = self.m.find_game("100")
        self.assertEqual(g["launch"]["source"], "override")
        self.assertTrue(g["launch"]["exe"].endswith("b.exe"))


class Env(Base):
    def test_build_env(self):
        os.environ["PROTON_USE_WINED3D"] = "1"  # inherited, must not leak
        try:
            gs = {"backend": "dxmt", "winedebug": "+steamclient", "env": "FOO=bar\n# c\nexport X='y z'"}
            paths = {"proton_ref": "/ref", "wine_dir": "/wine"}
            env = self.m.build_env("42", gs, paths)
        finally:
            del os.environ["PROTON_USE_WINED3D"]
        self.assertEqual(env["PROTON_USE_DXMT"], "1")
        self.assertNotIn("PROTON_USE_WINED3D", env)
        self.assertEqual((env["FOO"], env["X"], env["WINEDEBUG"]), ("bar", "y z", "+steamclient"))
        self.assertEqual((env["SteamAppId"], env["SteamGameId"], env["PROTON_LOG"]), ("42", "42", "1"))
        self.assertTrue(env["STEAM_COMPAT_DATA_PATH"].endswith("prefixes/42"))

    def test_env_errors(self):
        _, errs = self.m.parse_env_lines("1BAD=x\nnoequals")
        self.assertEqual(len(errs), 2)

    def test_vdf(self):
        d = self.m.parse_vdf('"a"\n{\n // c\n "b" "x \\"q\\""\n "c" { "d" "1" }\n}')
        self.assertEqual(d, {"a": {"b": 'x "q"', "c": {"d": "1"}}})


if __name__ == "__main__":
    unittest.main()
