#!/bin/sh
# Build the steamclient redirect test for x86_64 and i686 into steam/tests/redirect/out/<arch>/.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mingw=${LLVM_MINGW:-$here/../../../toolchains/llvm-mingw}
for arch in x86_64 i686; do
    o="$here/out/$arch"; mkdir -p "$o"
    dll=steamclient64.dll; [ "$arch" = i686 ] && dll=steamclient.dll
    "$mingw/bin/$arch-w64-mingw32-clang" -O1 -shared -o "$o/$dll" "$here/fake_steamclient.c"
    "$mingw/bin/$arch-w64-mingw32-clang" -O1 -o "$o/redirect_test.exe" "$here/redirect_test.c"
    echo "built $o: $dll redirect_test.exe"
done
