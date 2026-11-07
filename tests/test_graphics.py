import os

import pytest

import mproton_graphics as g
from conftest import make_gptk


def test_default_plan_matches_upstream():
    p = g.plan_graphics(g.DXVK)
    assert p.dxvk == ["d3d11", "d3d10core", "d3d9", "dxgi"]
    assert p.vkd3d == ["d3d12", "d3d12core"]
    assert p.builtin == ["d3d8"]
    assert p.dxmt == []
    assert all(p.overrides[d] == "n" for d in p.dxvk + p.vkd3d)
    assert p.stale == ["winemetal"]


def test_dxvk_without_dxgi_and_with_d3d8():
    p = g.plan_graphics(g.DXVK, use_dxvk_dxgi=False, use_dxvk_d3d8=True)
    assert "dxgi" in p.builtin and "dxgi" not in p.dxvk
    assert "d3d8" in p.dxvk


def test_wined3d_plan_matches_upstream():
    p = g.plan_graphics(g.WINED3D, use_dxvk_dxgi=False)
    assert p.builtin == ["d3d12", "d3d11", "d3d10", "d3d10core", "d3d10_1", "d3d9", "dxgi", "d3d8"]
    assert p.dxvk == [] and p.vkd3d == [] and p.overrides == {}


def test_dxmt_plan():
    p = g.plan_graphics(g.DXMT)
    assert set(p.dxmt) == {"d3d11", "d3d10core", "dxgi", "winemetal"}
    assert p.dxvk == ["d3d9"]  # D3D9 stays on DXVK
    assert p.vkd3d == ["d3d12", "d3d12core"]  # D3D12 stays on vkd3d-proton
    assert p.overrides["d3d11"] == "n" and p.overrides["winemetal"] == "n"
    assert p.stale == []


def test_d3dmetal_plan():
    p = g.plan_graphics(g.D3DMETAL, d3dmetal_dir="/gptk/lib/wine")
    assert p.dxvk == [] and p.vkd3d == [] and p.dxmt == []
    for dll in ("d3d12", "d3d11", "d3d10", "dxgi"):
        assert dll in p.builtin
        assert p.overrides[dll] == "b"
    assert p.dllpath_prepend == ["/gptk/lib/wine"]


@pytest.mark.parametrize("config,expected", [
    (set(), g.DXVK),
    ({"dxmt"}, g.DXMT),
    ({"wined3d"}, g.WINED3D),
    ({"wined3d", "dxmt"}, g.WINED3D),
])
def test_select_backend(config, expected):
    assert g.select_backend(config, {}, "aarch64-windows") == (expected, None)


def test_d3dmetal_requires_user_path():
    logs = []
    assert g.select_backend({"d3dmetal"}, {}, "aarch64-windows", logs.append) == (g.DXVK, None)
    assert "PROTON_D3DMETAL_PATH" in logs[0]


def test_d3dmetal_falls_back_to_next_requested():
    assert g.select_backend({"d3dmetal", "dxmt"}, {}, "aarch64-windows")[0] == g.DXMT


@pytest.mark.parametrize("sub", ["", "redist", "redist/lib"])
def test_find_d3dmetal_layouts(tmp_path, sub):
    make_gptk(str(tmp_path))
    wine_dir, reason = g.find_d3dmetal(os.path.join(str(tmp_path), sub), "aarch64-windows")
    assert reason is None
    assert wine_dir == os.path.join(str(tmp_path), "redist/lib/wine")


def test_d3dmetal_arch_mismatch_is_rejected(tmp_path):
    # GPTK 2.x ships only x86_64 D3DMetal; an arm64 Wine cannot load it.
    make_gptk(str(tmp_path), "x86_64-windows", "x86_64-unix")
    wine_dir, reason = g.find_d3dmetal(str(tmp_path), "aarch64-windows")
    assert wine_dir is None and "aarch64-windows" in reason
    assert g.find_d3dmetal(str(tmp_path), "x86_64-windows")[1] is None


def test_d3dmetal_missing_framework(tmp_path):
    assert g.find_d3dmetal(str(tmp_path), "aarch64-windows")[0] is None
