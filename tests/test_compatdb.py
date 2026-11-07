"""Backend -> compatdb rules mapping (launcher/mproton_compatdb.py), no launcher globals."""

import os

import pytest

import mproton_compatdb as c
import mproton_graphics as g
from mproton_runtime import Mach9Bundle
from conftest import make_mach9_bundle


@pytest.fixture
def bundle(tmp_path):
    return Mach9Bundle(make_mach9_bundle(str(tmp_path / "wine")))


@pytest.fixture
def full_bundle(tmp_path):
    root = make_mach9_bundle(str(tmp_path / "wine"), dxgi=("dxmt", "wined3d", "dxvk", "d3dmetal"),
                             d3d9=("mtld3d", "wined3d", "dxvk"))
    return Mach9Bundle(root)


def test_escape_matches_crate_display():
    # crate docs, "Format": %, ; and ASCII controls as uppercase %XX; = , space and UTF-8 as is
    assert c.escape("a;b") == "a%3Bb"
    assert c.escape("100%") == "100%25"
    assert c.escape("x\ny\r\t\x7f") == "x%0Ay%0D%09%7F"
    assert c.escape("n,b = é") == "n,b = é"


def test_rule_fields_in_key_order():
    r = c.Rule("Game.exe")
    r.add("env", "VKD3D_CONFIG=a;b")
    r.add("dll_overrides", "d3d11=n")
    r.set("dpi_aware", "false")
    r.set("d3d9", "wined3d")
    r.set("dxgi", "dxmt")
    assert r.to_line() == ("exe=Game.exe;dxgi=dxmt;d3d9=wined3d;dpi_aware=false;"
                           "dll_overrides=d3d11=n;env=VKD3D_CONFIG=a%3Bb")


def test_table_text_has_no_trailing_newline_and_skips_empty_rules():
    rules = [c.Rule("*").set("dxgi", "wined3d"), c.Rule("Other.exe"), c.Rule("steam:440").set("d3d9", "wined3d")]
    assert c.table_text(rules) == "exe=*;dxgi=wined3d\nexe=steam:440;d3d9=wined3d"
    assert c.table_text([c.Rule("*")]) == ""


# backend, compat flags, extra env -> expected (dxgi, d3d9) on the * rule
MAPPING = [
    (g.MACH9, set(), {}, (None, None)),          # built-in rules decide: dxmt + mtld3d
    (g.DXMT, set(), {}, ("dxmt", None)),         # D3D9 keeps mtld3d
    (g.WINED3D, set(), {}, ("wined3d", "wined3d")),
    (g.DXVK, set(), {}, ("dxvk", "dxvk")),
    (g.D3DMETAL, set(), {}, ("d3dmetal", None)),
    (g.MACH9, {"mtld3d"}, {}, (None, "mtld3d")),
    (g.DXMT, set(), {"MPROTON_D3D9": "wined3d"}, ("dxmt", "wined3d")),
    (g.WINED3D, set(), {"MPROTON_DXGI": "DXMT"}, ("dxmt", "wined3d")),
    (g.WINED3D, set(), {"MPROTON_DXGI": "default"}, ("default", "wined3d")),
]


@pytest.mark.parametrize("backend,flags,env,expect", MAPPING)
def test_backend_to_trees(full_bundle, backend, flags, env, expect):
    rules, trees = c.build_rules(backend, flags, env, full_bundle, {})
    every = rules[0]
    assert every.get("exe") == "*"
    assert (every.get("dxgi"), every.get("d3d9")) == expect
    assert trees == {k: v for k, v in zip(("dxgi", "d3d9"), expect) if v}


def test_missing_tree_keeps_builtin(bundle):
    logs = []
    rules, trees = c.build_rules(g.DXVK, set(), {}, bundle, {}, logs.append)
    assert trees == {}
    assert rules[0].get("dxgi") is None and rules[0].get("d3d9") is None
    assert any("dxgi=dxvk requested" in m and "lib/wine/dxgi/dxvk/" in m for m in logs)


def test_dxvk_dxgi_tree_only(tmp_path):
    b = Mach9Bundle(make_mach9_bundle(str(tmp_path / "w"), dxgi=("dxmt", "wined3d", "dxvk")))
    _, trees = c.build_rules(g.DXVK, set(), {}, b, {})
    assert trees == {"dxgi": "dxvk"}  # D3D9 stays on mtld3d until d3d9/dxvk exists


def test_tree_needs_the_host_pe_dir(tmp_path):
    root = make_mach9_bundle(str(tmp_path / "w"))
    os.makedirs(os.path.join(root, "lib/wine/dxgi/dxvk/i386-windows"))
    b = Mach9Bundle(root)
    assert not b.has_tree("dxgi", "dxvk")
    assert b.trees("dxgi") == ["dxmt", "wined3d"]


def test_dll_overrides_keep_upstream_order(bundle):
    rules, _ = c.build_rules(g.MACH9, set(), {"WINEDLLOVERRIDES": "foo=n,b;;bar;baz,qux="}, bundle,
                             {"steam.exe": "b", "d3d12": ""})
    assert rules[0].get("dll_overrides") == ["foo=n,b", "baz,qux=", "steam.exe=b", "d3d12="]


@pytest.mark.parametrize("flags,expect", [({"forcelgadd"}, "true"), ({"noforcelgadd"}, "false"), (set(), None)])
def test_large_address_aware(bundle, flags, expect):
    rules, _ = c.build_rules(g.MACH9, flags, {}, bundle, {})
    assert rules[0].get("large_address_aware") == expect


def test_metal_hud(bundle):
    rules, _ = c.build_rules(g.MACH9, set(), {"MPROTON_METAL_HUD": "1"}, bundle, {})
    assert rules[0].get("metal_hud") == "true"


def test_explicit_backend_also_targets_the_steam_app(full_bundle):
    rules, _ = c.build_rules(g.WINED3D, set(), {"SteamAppId": "12210"}, full_bundle, {})
    assert [r.to_line() for r in rules[1:]] == ["exe=steam:12210;dxgi=wined3d;d3d9=wined3d"]


@pytest.mark.parametrize("backend,appid", [(g.MACH9, "12210"), (g.WINED3D, "0"), (g.WINED3D, "abc"),
                                           (g.WINED3D, "4294967296"), (g.WINED3D, "")])
def test_no_steam_app_rule(full_bundle, backend, appid):
    rules, _ = c.build_rules(backend, set(), {"SteamAppId": appid}, full_bundle, {})
    assert len(rules) == 1


def test_steam_app_id_is_normalised():
    assert c.steam_app_id({"SteamAppId": "0440"}) == "440"


def test_write_rules_file_is_atomic(tmp_path):
    path = str(tmp_path / "cd" / "compatdb.rules")
    c.write_rules_file(path, "exe=*;dxgi=wined3d")
    c.write_rules_file(path, "exe=*;dxgi=dxmt")
    assert open(path).read() == "exe=*;dxgi=dxmt"
    assert os.listdir(tmp_path / "cd") == ["compatdb.rules"]


def test_apply_env_drops_inherited_conflicts(tmp_path):
    env = {"WINEDLLOVERRIDES": "x=n", "WINE_LARGE_ADDRESS_AWARE": "1", "WINE_COMPATDB_FILE": "/old",
           "WINE_PROF_SERVICE": "evil", "WINE_COMPATDB": "exe=Game.exe;dxgi=wined3d"}
    c.apply_env(env, str(tmp_path / "r"))
    assert env == {"WINE_COMPATDB_FILE": str(tmp_path / "r"), "WINE_COMPATDB": "exe=Game.exe;dxgi=wined3d"}
    env = {"MPROTON_PROF_SERVICE": "com.example.prof"}
    c.apply_env(env, str(tmp_path / "r"))
    assert env["WINE_PROF_SERVICE"] == "com.example.prof"


def test_select_backend_mach9_default():
    assert g.select_backend(set(), {}, "aarch64-windows", default=g.MACH9) == (g.MACH9, None)
    assert g.select_backend({"dxvk", "dxmt"}, {}, "aarch64-windows", default=g.MACH9) == (g.DXMT, None)
    found = lambda env, arch: ("/tree", None)
    missing = lambda env, arch: (None, "no tree")
    assert g.select_backend({"d3dmetal"}, {}, "aarch64-windows", default=g.MACH9,
                            d3dmetal_finder=found) == (g.D3DMETAL, "/tree")
    assert g.select_backend({"d3dmetal"}, {}, "aarch64-windows", default=g.MACH9,
                            d3dmetal_finder=missing) == (g.MACH9, None)
