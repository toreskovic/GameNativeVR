#!/usr/bin/env bash
set -euo pipefail
repository="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
${CXX:-c++} -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repository/tools/vr-ffr/test_includes" "$repository/tools/vr-ffr/test_layer.cpp" -o "$output/test-layer"
"$output/test-layer"

${CXX:-c++} -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$repository/tools/vr-ffr/test_pass_history.cpp" -o "$output/test-pass-history"
"$output/test-pass-history"
