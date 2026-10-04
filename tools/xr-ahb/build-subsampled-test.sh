#!/usr/bin/env bash
set -euo pipefail
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to the Android NDK directory}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-/tmp/gn-subsampled-test}"
mkdir -p "$out"
compiler="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
glslangValidator -V --vn interopVertex "$root/tools/xr-ahb/subsampled-test.vert" -o "$out/interop-vertex.h"
glslangValidator -V --vn interopPattern "$root/tools/xr-ahb/subsampled-pattern.frag" -o "$out/interop-pattern.h"
glslangValidator -V --vn interopReadback "$root/tools/xr-vulkan/subsampled-readback.frag" -o "$out/interop-readback.h"
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -static-libstdc++)
"$compiler" "${flags[@]}" -I"$out" "$root/tools/xr-ahb/subsampled-interop.cpp" -ldl -landroid -o "$out/gn-subsampled-interop"
"$compiler" "${flags[@]}" "$root/tools/xr-ahb/subsampled-capability-probe.cpp" -ldl -o "$out/gn-subsampled-capability-probe"
