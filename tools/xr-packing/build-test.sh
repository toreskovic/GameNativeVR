#!/usr/bin/env bash
set -euo pipefail
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-/tmp/gn-packing-test}"
bash "$root/tools/xr-ahb/build-subsampled-test.sh" "$out"
ndk="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
"$ndk/aarch64-linux-android29-clang" -O2 -Wall -Wextra -Werror -c "$root/tools/xr-packing/test-pack.c" -o "$out/pack.o"
"$ndk/aarch64-linux-android29-clang++" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -static-libstdc++ -I"$out" "$root/tools/xr-packing/test.cpp" "$out/pack.o" -ldl -landroid -o "$out/gn-packing-test"

"$ndk/aarch64-linux-android29-clang++" -std=c++17 -DGN_TEST_WIDTH=513 -DGN_TEST_HEIGHT=517 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -static-libstdc++ -I"$out" "$root/tools/xr-packing/test.cpp" "$out/pack.o" -ldl -landroid -o "$out/gn-packing-odd-test"
