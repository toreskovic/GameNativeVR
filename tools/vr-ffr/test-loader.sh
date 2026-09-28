#!/usr/bin/env bash
set -euo pipefail
repository="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
${CXX:-c++} -std=c++17 -O1 -shared -fPIC -fvisibility=hidden -Wl,-Bsymbolic \
  -I"$repository/tools/vr-ffr/test_includes" "$repository/app/src/main/cpp/vrffr/layer.cpp" -o "$output/libffr.so"
cat > "$output/layer.json" <<JSON
{"file_format_version":"1.2.0","layer":{"name":"VK_LAYER_GN_vr_foveation","type":"GLOBAL","library_path":"$output/libffr.so","api_version":"1.3.0","implementation_version":"1","description":"FFR loader smoke test"}}
JSON
${CXX:-c++} -std=c++17 "$repository/tools/vr-ffr/test_loader.cpp" -lvulkan -o "$output/test-loader"
VK_LAYER_PATH="$output" GN_VR_FFR=1 "$output/test-loader"
