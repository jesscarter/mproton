"""Runs steam/tests/check_abi.py (lsteamclient struct layouts on arm64 macOS).

Skipped unless the libclang 15 Python bindings (pip install libclang==15.0.6.1,
the version upstream Proton generates with) and a clang driver are available.
Takes about two minutes.
"""

import ctypes
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECK = os.path.join(ROOT, "steam", "tests", "check_abi.py")


def _libclang_major():
    try:
        import clang.cindex
        lib = ctypes.CDLL(clang.cindex.conf.get_filename())
    except Exception:
        return None

    class CXString(ctypes.Structure):
        _fields_ = [("data", ctypes.c_void_p), ("flags", ctypes.c_uint)]

    lib.clang_getClangVersion.restype = CXString
    lib.clang_getCString.restype = ctypes.c_char_p
    lib.clang_getCString.argtypes = [CXString]
    version = lib.clang_getCString(lib.clang_getClangVersion()).decode()
    # "clang version 15.0.6 (...)"
    for word in version.split():
        if word[:1].isdigit():
            return int(word.split(".")[0])
    return None


def _skip_reason():
    major = _libclang_major()
    if major is None:
        return "libclang Python bindings not installed (pip install libclang==15.0.6.1)"
    if major != 15:
        return "gen_wrapper.py needs libclang 15, found %d (pip install libclang==15.0.6.1)" % major
    if not shutil.which(os.environ.get("CLANG", "clang")):
        return "no clang driver on PATH (set CLANG)"
    return None


@pytest.mark.skipif(_skip_reason() is not None, reason=_skip_reason() or "")
def test_lsteamclient_abi():
    proc = subprocess.run([sys.executable, CHECK], capture_output=True, text=True)
    print(proc.stdout)
    print(proc.stderr)
    assert proc.returncode == 0, proc.stdout[-4000:] + proc.stderr[-2000:]
