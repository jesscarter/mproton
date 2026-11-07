# DXVK patches for KosmicKrisp

Empty on purpose. `build.sh` applies every `*.patch` here, in name order, with `git apply`.

The audit in [docs/graphics-dxvk.md](../../../docs/graphics-dxvk.md) found nothing in DXVK
v3.1.1 to patch for KosmicKrisp:

- every feature DXVK requires is in KosmicKrisp's feature list (`check-features.py`);
- DXVK already falls back cleanly when shaderFloat64, VK_EXT_graphics_pipeline_library and
  VK_EXT_descriptor_buffer / VK_EXT_descriptor_heap are missing;
- DXVK already detects KosmicKrisp's driver ID and turns tiler mode on.

Add a patch only for a KosmicKrisp-specific problem seen on a Mac. Name it
`NNNN-short-description.patch` (`git format-patch` output against the pinned commit), and add
a line here saying what it fixes and when it can be dropped. Applying any patch makes DXVK
report its version with a trailing `+` (`git describe --dirty=+`), and build.sh records each
patch's hash in the output's `PATCHES` file.
