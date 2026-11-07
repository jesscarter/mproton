Minimal C library stubs for `gen_wrapper.py` (mproton).

The Steamworks SDK headers include `<stdio.h>`, `<string.h>`, `<assert.h>` and
`<stdint.h>`. Upstream parses them against the build host's glibc
(`/usr/include/x86_64-linux-gnu`, which also needs the i386 multilib headers).
That cannot work for an `arm64-apple-macos` parse on Linux, or for any parse on
a macOS host. These stubs declare only what the SDK headers use; struct layouts
do not depend on them. `<stdint.h>`, `<stddef.h>` and `<stdarg.h>` come from
clang's own resource directory, so the integer types follow each target.
