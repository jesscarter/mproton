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


class Depots(Base):
    # Shape of api.steamcmd.net data for Hypernet Explorer (4193010), 2026-11-20.
    HYPERNET = {"common": {"name": "Hypernet Explorer", "type": "Game", "oslist": "windows"},
                "depots": {"228989": {"config": {"oslist": "windows"}, "depotfromapp": "228980",
                                      "sharedinstall": "1"},
                           "4193011": {"config": {"osarch": "64", "oslist": "windows"},
                                       "manifests": {"public": {"gid": "1", "size": "500000000"},
                                                     "beta": {"gid": "2"}}},
                           "baselanguages": "english", "branches": {"public": {}}}}

    def test_hypernet(self):
        self.infos["4193010"] = self.HYPERNET
        d = self.m.depot_lookup("4193010")
        self.assertEqual(d["commands"], ["download_depot 4193010 4193011"])
        self.assertEqual((d["name"], d["mac"]), ("Hypernet Explorer", False))
        skipped = [e for e in d["depots"] if not e["needed"]]
        self.assertEqual([e["depot"] for e in skipped], ["228989"])
        self.assertIn("redistributable", skipped[0]["why"])
        self.assertEqual(d["depots"][0]["size"], 500000000)
        self.assertFalse(d["depots"][0]["downloaded"])
        (self.m.CONTENT_DIR / "app_4193010/depot_4193011").mkdir(parents=True)
        self.assertTrue(self.m.depot_lookup("4193010")["depots"][0]["downloaded"])

    def test_mixed(self):
        pub = {"manifests": {"public": {"gid": "1", "size": "10"}}}
        info = {"depots": {
            "11": dict(pub, config={"oslist": "macos"}),                       # Mac only: hidden
            "12": dict(pub, config={"oslist": "windows", "osarch": "64"}),     # needed
            "13": dict(pub, config={"oslist": "windows", "osarch": "32"}),     # 32-bit twin
            "14": dict(pub),                                                   # all platforms
            "15": dict(pub, config={"language": "german"}),                    # other language
            "16": dict(pub, config={"language": "english"}),                   # needed
            "17": dict(pub, dlcappid="99"),                                    # DLC
            "18": {"config": {"oslist": "windows"}, "manifests": {"beta": {"gid": "3"}}},  # no public
            "19": dict(pub, config={"lowviolence": "1"}),
        }}
        deps = {e["depot"]: e for e in self.m.windows_depots(info)}
        self.assertNotIn("11", deps)
        self.assertEqual(sorted(k for k, e in deps.items() if e["needed"]), ["12", "14", "16"])
        self.assertIn("32-bit", deps["13"]["why"])
        self.assertIn("german", deps["15"]["why"])
        self.assertIn("DLC", deps["17"]["why"])
        self.assertIn("public", deps["18"]["why"])
        self.assertIn("low-violence", deps["19"]["why"])

    def test_only_32bit_is_needed(self):
        info = {"depots": {"5": {"config": {"oslist": "windows", "osarch": "32"},
                                 "manifests": {"public": {"gid": "1"}}}}}
        self.assertTrue(self.m.windows_depots(info)[0]["needed"])

    def test_search_by_appid_and_name(self):
        self.infos["4193010"] = self.HYPERNET
        r = self.m.store_search(" 4193010 ")
        self.assertEqual((r[0]["appid"], r[0]["windows"], r[0]["mac"]), ("4193010", True, False))
        self.assertEqual(self.m.store_search("123"), [])
        self.assertEqual(self.m.store_search("  "), [])
        seen = []
        self.m._fetch_json = lambda url: seen.append(url) or {"items": [
            {"type": "app", "id": 4193010, "name": "Hypernet Explorer",
             "platforms": {"windows": True, "mac": False, "linux": False}},
            {"type": "sub", "id": 5, "name": "a package"}]}
        r = self.m.store_search("hypernet explorer")
        self.assertEqual([x["appid"] for x in r], ["4193010"])
        self.assertIn("term=hypernet+explorer", seen[0])

    def test_lookup_offline(self):
        with self.assertRaises(RuntimeError):
            self.m.depot_lookup("1")


if __name__ == "__main__":
    unittest.main()
