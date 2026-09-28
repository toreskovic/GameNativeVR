#!/usr/bin/env bash
set -euo pipefail
repository="$(cd "$(dirname "$0")/../.." && pwd)"
ndk="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to an Android NDK installation}"
cmake -S "$repository/app/src/main/cpp/vrffr" -B "$repository/app/build/vr-ffr" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build "$repository/app/build/vr-ffr" --parallel
cmake -S "$repository/app/src/main/windows/openxr_runtime/unix" -B "$repository/app/build/vr-ffr-bridge" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build "$repository/app/build/vr-ffr-bridge" --parallel
for variant in legacyXr modernXr; do
  cp "$repository/app/build/vr-ffr/libgn_vrffr_probe.so" "$repository/app/src/$variant/jniLibs/arm64-v8a/"
  cp "$repository/app/build/vr-ffr/libVkLayer_GN_vr_foveation.so" "$repository/app/src/$variant/jniLibs/arm64-v8a/"
  cp "$repository/app/build/vr-ffr-bridge/gamenative_xr_unixbridge.so" "$repository/app/src/$variant/assets/"
done
