"""Locating the Wine runtime the mproton launcher drives.

Two layouts exist:

* **Proton dist** (``files/bin``, ``files/lib/wine``, ``files/share/default_pfx``):
  what upstream Proton ships, and what the launcher still uses on Linux.
* **mach9 bundle** (athei/mach9 ``dist/wine``): ``bin/wine`` is a symlink into
  ``lib/wine/aarch64-unix/wine.app``, Direct3D lives in per-implementation trees
  (``lib/wine/dxgi/<impl>/``, ``lib/wine/d3d9/<impl>/``) chosen per process by
  ``compatdb.so``, and there is no default prefix: ``wineboot -i`` makes one.

On Darwin the launcher looks for a mach9 bundle first (``find_bundle``) and
falls back to the Proton layout only when none is found.  On Linux a mach9
bundle (arm64 Mach-O) cannot run, so the Proton layout is always used.

Nothing here touches the filesystem at import time.
"""

import os

# Present in every mach9 bundle: Wine's ntdll dlopens it in every process.
COMPATDB_SO = "lib/wine/aarch64-unix/compatdb.so"

# The bundle is arm64 only: 64-bit PE modules are ARM64X in aarch64-windows,
# wow64 ones in i386-windows.
HOST_PE_ARCH = "aarch64-windows"
WOW64_PE_ARCH = "i386-windows"

# Direct3D families and the trees mach9 ships today (docs/compatdb.md).
DXGI = "dxgi"
D3D9 = "d3d9"
FAMILIES = (DXGI, D3D9)


def is_bundle(path):
    """True if ``path`` is the root of a mach9 bundle (``dist/wine``)."""
    if not path:
        return False
    # bin/wine is a symlink into wine.app; accept it even if dangling, so a
    # bundle copied without wine.app still gets a clear error from Wine itself.
    return (os.path.lexists(os.path.join(path, "bin", "wine")) and
            os.path.isfile(os.path.join(path, COMPATDB_SO)))


def _candidates(base_dir, environ):
    explicit = environ.get("MPROTON_WINE_DIR")
    if explicit:
        explicit = os.path.abspath(os.path.expanduser(explicit))
        # The bundle root itself, or a directory holding it as wine/ (mach9's
        # dist/ or an app's Resources/).
        return True, [explicit, os.path.join(explicit, "wine")]
    base_dir = os.path.abspath(base_dir)
    return False, [os.path.join(base_dir, "wine"), os.path.join(base_dir, "files")]


def find_bundle(base_dir, environ):
    """Find a mach9 bundle for a launcher installed at ``base_dir``.

    Search order: ``$MPROTON_WINE_DIR`` (the bundle, or a directory with the
    bundle as ``wine/``), then ``<launcher dir>/wine``, then
    ``<launcher dir>/files`` (a bundle copied in place of Proton's ``files/``).

    Returns ``(Mach9Bundle, None)``, or ``(None, reason)``.  ``reason`` is
    only meaningful when ``MPROTON_WINE_DIR`` was set: then the caller must
    not fall back silently.
    """
    explicit, candidates = _candidates(base_dir, environ)
    for cand in candidates:
        if is_bundle(cand):
            return Mach9Bundle(cand), None
    if explicit:
        return None, ("MPROTON_WINE_DIR=%s is not a mach9 bundle (expected bin/wine and %s)"
                      % (environ["MPROTON_WINE_DIR"], COMPATDB_SO))
    return None, None


class Mach9Bundle:
    """Paths inside a mach9 bundle (docs/building.md, "Bundle layout")."""

    def __init__(self, root):
        self.root = os.path.abspath(root).rstrip("/") + "/"

    def path(self, d):
        return self.root + d

    @property
    def bin_dir(self):
        return self.path("bin/")

    @property
    def wine_bin(self):
        return self.bin_dir + "wine"

    @property
    def wineserver_bin(self):
        return self.bin_dir + "wineserver"

    @property
    def lib_wine(self):
        return self.path("lib/wine/")

    @property
    def icd_dir(self):
        """KosmicKrisp's manifest dir; ntdll points VK_DRIVER_FILES here itself."""
        return self.path("share/vulkan/icd.d/")

    def pe_dir(self, wow64=False):
        return self.lib_wine + (WOW64_PE_ARCH if wow64 else HOST_PE_ARCH) + "/"

    def tree_dir(self, family, impl):
        return self.lib_wine + family + "/" + impl + "/"

    def has_tree(self, family, impl, pe_arch=HOST_PE_ARCH):
        """Whether ``lib/wine/<family>/<impl>/<pe_arch>`` exists.

        compatdb.so checks exactly this directory before it prepends a tree
        (``apply::tree``); without it the process gets the default dir's
        fake-module markers, i.e. no Direct3D for that family.
        """
        return os.path.isdir(self.tree_dir(family, impl) + pe_arch)

    def trees(self, family):
        """Implementations present for ``family``, sorted."""
        base = self.lib_wine + family
        try:
            names = os.listdir(base)
        except OSError:
            return []
        return sorted(n for n in names if self.has_tree(family, n))

    def __repr__(self):
        return "Mach9Bundle(%r)" % self.root
