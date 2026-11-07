"""Offline checks of build/mproton-build.sh (the macOS steps can't run here)."""
import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "build", "mproton-build.sh")


def series():
    with open(os.path.join(ROOT, "build", "mach9-patches", "series")) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def test_series_files_exist():
    entries = series()
    assert entries, "the series is empty"
    for p in entries:
        assert os.path.isfile(os.path.join(ROOT, p)), p


def test_patches_in_dir_are_in_series():
    d = os.path.join(ROOT, "build", "mach9-patches")
    listed = {os.path.basename(p) for p in series()}
    for f in os.listdir(d):
        if f.endswith(".patch"):
            assert f in listed, f"{f} is not in build/mach9-patches/series"


def test_pins_are_full_hashes():
    with open(os.path.join(ROOT, "build", "pins.env")) as f:
        pins = dict(re.findall(r"^([A-Z0-9_]+_COMMIT)=([0-9a-f]+)", f.read(), re.M))
    assert set(pins) >= {"MACH9_COMMIT", "WINE_COMMIT", "FEX_COMMIT", "MESA_COMMIT",
                         "DXMT_COMMIT", "VKD3D_PROTON_COMMIT"}
    for k, v in pins.items():
        assert len(v) == 40, k


def run(*args, **env):
    e = dict(os.environ, MPROTON_WORK=env.pop("work"))
    return subprocess.run([SCRIPT, *args], capture_output=True, text=True, env=e)


def test_help_and_bad_step(tmp_path):
    r = run("--help", work=str(tmp_path))
    assert r.returncode == 0 and "Steps, in order" in r.stdout
    r = run("bogus", work=str(tmp_path))
    assert r.returncode == 2 and "unknown step" in r.stderr


def test_status_on_empty_workspace(tmp_path):
    r = run("--status", work=str(tmp_path))
    assert r.returncode == 0
    assert "mach9     not done" in r.stdout and "bundle    not done" in r.stdout


@pytest.mark.skipif(os.uname().sysname == "Darwin", reason="checks the non-mac refusal")
def test_mac_steps_refuse_off_mac(tmp_path):
    r = run("wine", work=str(tmp_path))
    assert r.returncode == 1 and "needs macOS" in r.stdout


@pytest.mark.skipif(shutil.which("shellcheck") is None, reason="shellcheck not installed")
def test_shellcheck_clean():
    r = subprocess.run(["shellcheck", "-x", SCRIPT, os.path.join(ROOT, "build", "lib", "common.sh")],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout
