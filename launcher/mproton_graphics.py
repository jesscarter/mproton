"""Graphics backend selection and DLL placement plan for the mproton launcher.

Backends (chosen per game via env var or STEAM_COMPAT_CONFIG flag):

  dxvk      default. D3D8(opt)/9/10/11 -> DXVK, D3D12 -> vkd3d-proton, on
            Vulkan (KosmicKrisp on macOS).  Identical to upstream Proton.
  dxmt      PROTON_USE_DXMT=1 / 'dxmt'.  D3D10/11 + DXGI -> DXMT (Metal).
            D3D9 stays on DXVK, D3D12 stays on vkd3d-proton.
  wined3d   PROTON_USE_WINED3D=1 / 'wined3d' (upstream behaviour).
  d3dmetal  PROTON_USE_D3DMETAL=1 / 'd3dmetal', bring-your-own: only activates
            when PROTON_D3DMETAL_PATH points at an installed Game Porting
            Toolkit whose D3DMetal build matches the running Wine's arch.
            Nothing from GPTK is shipped or copied by mproton; Wine is
            pointed at the user's install through WINEDLLPATH.

  mach9     the default on a mach9 bundle: no tree chosen by the launcher, so
            compatdb's built-in rules decide (DXMT + vkd3d-proton for
            D3D10/11/12, mtld3d for D3D9).  On a mach9 bundle the other
            backends become compatdb rules, not DLL copies; see
            mproton_compatdb.py.

Precedence when several are requested: wined3d > d3dmetal > dxmt > dxvk.
(wined3d is the debugging "safe mode", as upstream lets it override all.)
"""

import os

DXVK = "dxvk"
DXMT = "dxmt"
WINED3D = "wined3d"
D3DMETAL = "d3dmetal"
MACH9 = "mach9"

BACKENDS = (DXVK, DXMT, WINED3D, D3DMETAL, MACH9)

# env var -> compat_config flag, in the style of PROTON_USE_WINED3D
BACKEND_ENV_VARS = [
    ("PROTON_USE_DXMT", DXMT),
    ("PROTON_USE_D3DMETAL", D3DMETAL),
    ("PROTON_USE_DXVK", DXVK),
    # per-family / per-API switches (mach9 bundle only)
    ("PROTON_USE_MTLD3D", "mtld3d"),
    ("PROTON_NO_D3D12", "nod3d12"),
]

D3DMETAL_DLLS = ["d3d12", "d3d11", "d3d10", "dxgi"]

# Unix arch dir that matches a PE arch (for the D3DMetal .so side).
_UNIX_ARCH_FOR_PE = {
    "x86_64-windows": "x86_64-unix",
    "aarch64-windows": "aarch64-unix",
}


class GraphicsPlan:
    """Which DLLs come from where, and the overrides/env that go with them.

    Each list holds DLL basenames without '.dll'.  ``builtin`` DLLs are copied
    from Proton's default prefix (Wine builtins: wined3d-backed or placeholders);
    ``dxvk``/``vkd3d``/``dxmt`` are copied from files/lib/wine/<dir>/<pe-arch>/.
    """

    def __init__(self, backend):
        self.backend = backend
        self.builtin = []
        self.dxvk = []
        self.vkd3d = []
        self.dxmt = []
        self.overrides = {}
        self.dllpath_prepend = []
        self.env = {}
        self.stale = []  # DLLs left behind by other backends, to remove

    def __repr__(self):
        return ("GraphicsPlan(%s, builtin=%r, dxvk=%r, vkd3d=%r, dxmt=%r, overrides=%r)" %
                (self.backend, self.builtin, self.dxvk, self.vkd3d, self.dxmt, self.overrides))


def find_d3dmetal(path, host_pe_arch):
    """Validate a user-supplied GPTK location.

    Accepts the GPTK ``lib`` dir itself or a parent containing ``lib/`` or
    ``redist/lib/``.  The expected layout (GPTK 2.x redist) is::

        lib/external/D3DMetal.framework
        lib/external/libd3dshared.dylib
        lib/wine/<arch>-windows/{d3d12,d3d11,d3d10,dxgi}.dll
        lib/wine/<arch>-unix/{d3d12,d3d11,d3d10,dxgi}.so

    Returns (wine_dir, None) on success or (None, reason) on failure.
    """
    if not path:
        return None, "PROTON_D3DMETAL_PATH is not set"
    path = os.path.expanduser(path)
    for cand in (path, os.path.join(path, "lib"), os.path.join(path, "redist", "lib")):
        if os.path.isdir(os.path.join(cand, "external", "D3DMetal.framework")):
            lib = cand
            break
    else:
        return None, "no external/D3DMetal.framework under " + path

    wine_dir = os.path.join(lib, "wine")
    unix_arch = _UNIX_ARCH_FOR_PE.get(host_pe_arch)
    pe_dir = os.path.join(wine_dir, host_pe_arch)
    if not os.path.isfile(os.path.join(pe_dir, "d3d12.dll")):
        return None, "GPTK has no %s build of D3DMetal (looked in %s)" % (host_pe_arch, pe_dir)
    if unix_arch is None or not os.path.isfile(os.path.join(wine_dir, unix_arch, "d3d12.so")):
        return None, "GPTK has no %s unix side for D3DMetal" % unix_arch
    return wine_dir, None


def select_backend(compat_config, env, host_pe_arch, log=lambda msg: None,
                   default=DXVK, d3dmetal_finder=None):
    """Pick the backend from compat_config flags; returns (backend, d3dmetal_dir).

    ``default`` is DXVK on a Proton dist and MACH9 on a mach9 bundle.
    ``d3dmetal_finder(env, host_pe_arch)`` returns (location, reason); it
    defaults to validating PROTON_D3DMETAL_PATH (Proton dist).
    """
    if d3dmetal_finder is None:
        d3dmetal_finder = lambda e, arch: find_d3dmetal(e.get("PROTON_D3DMETAL_PATH"), arch)
    wanted = [b for b in (WINED3D, D3DMETAL, DXMT, DXVK) if b in compat_config]
    if len(wanted) > 1:
        log("Multiple graphics backends requested (%s), using %s" % (", ".join(wanted), wanted[0]))

    for backend in wanted:
        if backend == D3DMETAL:
            wine_dir, reason = d3dmetal_finder(env, host_pe_arch)
            if wine_dir is None:
                log("D3DMetal requested but unavailable: " + reason + "; falling back")
                continue
            return D3DMETAL, wine_dir
        return backend, None
    return default, None


def plan_graphics(backend, use_dxvk_dxgi=True, use_dxvk_d3d8=False, d3dmetal_dir=None):
    """Build the DLL placement plan.  Mirrors upstream's dxvkfiles/wined3dfiles."""
    plan = GraphicsPlan(backend)

    if backend == WINED3D:
        plan.builtin = ["d3d12", "d3d11", "d3d10", "d3d10core", "d3d10_1", "d3d9"]
    elif backend == DXMT:
        plan.dxmt = ["d3d11", "d3d10core", "winemetal"]
        plan.dxvk = ["d3d9"]
        plan.vkd3d = ["d3d12", "d3d12core"]
    elif backend == D3DMETAL:
        # Restore Wine's builtins in the prefix; Wine then resolves them as
        # builtin from WINEDLLPATH, where GPTK's dir is searched first.
        plan.builtin = ["d3d10core", "d3d10_1", "d3d9"] + list(D3DMETAL_DLLS)
        plan.dllpath_prepend = [d3dmetal_dir]
        for dll in D3DMETAL_DLLS:
            plan.overrides[dll] = "b"
        plan.env["MPROTON_D3DMETAL"] = "1"
    else:
        plan.dxvk = ["d3d11", "d3d10core", "d3d9"]
        plan.vkd3d = ["d3d12", "d3d12core"]

    # dxgi: whoever owns D3D11 owns DXGI (upstream: DXVK unless dxgi=b)
    if backend == DXMT:
        plan.dxmt.append("dxgi")
    elif backend == DXVK and use_dxvk_dxgi:
        plan.dxvk.append("dxgi")
    elif backend != D3DMETAL:
        plan.builtin.append("dxgi")

    if use_dxvk_d3d8:  # upstream honours dxvkd3d8 even with wined3d
        plan.dxvk.append("d3d8")
    else:
        plan.builtin.append("d3d8")

    for dll in plan.dxvk + plan.vkd3d + plan.dxmt:
        plan.overrides[dll] = "n"

    if backend != DXMT:
        plan.stale.append("winemetal")
    return plan
