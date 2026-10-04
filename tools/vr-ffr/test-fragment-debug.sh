#!/usr/bin/env bash
set -euo pipefail
repository="$(cd "$(dirname "$0")/../.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
cat > "$output/test.frag" <<'GLSL'
#version 450
layout(location=0) in vec4 color;
layout(location=0) out vec4 target;
layout(location=1) out vec4 other;
void main() {
    other=color;
    if (color.a<.1) discard;
    if (color.r>.5) { target=color; return; }
    target=vec4(color.bgr,color.a);
}
GLSL
${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror "$repository/tools/vr-ffr/test_fragment_debug.cpp" -o "$output/patch"
for version in vulkan1.0 vulkan1.1 vulkan1.2; do
    glslangValidator -V --target-env "$version" "$output/test.frag" -o "$output/source.spv"
    "$output/patch" "$output/source.spv" "$output/debug.spv"
    spirv-val --target-env "$version" "$output/debug.spv"
done
# DXVK's scalarized stores: there is deliberately no whole-vector output store.
cat > "$output/test.frag" <<'GLSL'
#version 450
layout(location=0) in vec4 color;
layout(location=0) out vec4 target;
void writeColor() {
    target.r=color.r; target.g=color.g; target.b=color.b; target.a=color.a;
}
void main() { writeColor(); }
GLSL
glslangValidator -V --target-env vulkan1.2 "$output/test.frag" -o "$output/source.spv"
"$output/patch" "$output/source.spv" "$output/debug.spv"
spirv-val --target-env vulkan1.2 "$output/debug.spv"
# Split color and alpha declarations using Component decorations.
cat > "$output/test.frag" <<'GLSL'
#version 450
layout(location=0) in vec4 color;
layout(location=0,component=0) out vec3 rgb;
layout(location=0,component=3) out float alpha;
void main() { rgb=color.rgb; alpha=color.a; }
GLSL
glslangValidator -V --target-env vulkan1.2 "$output/test.frag" -o "$output/source.spv"
"$output/patch" "$output/source.spv" "$output/debug.spv"
spirv-val --target-env vulkan1.2 "$output/debug.spv"
