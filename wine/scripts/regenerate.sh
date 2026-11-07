#!/bin/bash
# Regenerate the files Wine keeps generated in git, in a tree with the series applied:
# configure + include/config.h.in (autoconf/autoheader), the server protocol files
# (make_requests), spec-derived files (make_specfiles) and the Vulkan headers/thunks
# (make_vulkan, which downloads the vk.xml version it pins).
# usage: wine/scripts/regenerate.sh <wine-src>
set -eu
cd "${1:?usage: regenerate.sh <wine-src>}"
tools/make_requests
tools/make_specfiles
(cd dlls/winevulkan && XDG_CACHE_HOME=${XDG_CACHE_HOME:-${TMPDIR:-/tmp}} python3 ./make_vulkan)
autoconf -f
autoheader -f
rm -rf autom4te.cache configure~ include/config.h.in~
git status --short
