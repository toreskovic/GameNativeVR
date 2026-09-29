#pragma once
#include <algorithm>
#include <cmath>

namespace xrimmersive::windowsvr {
// The game sees a centered crop in tangent space, not a multiplier of angles.
inline void scaleFovPair(float &a, float &b, float scale) {
    const float ta = std::tan(a), tb = std::tan(b);
    const float center = (ta + tb) * 0.5f;
    a = std::atan(center + (ta - center) * scale);
    b = std::atan(center + (tb - center) * scale);
}
inline unsigned scaledFovDimension(unsigned size, float scale) {
    return std::max(2u, static_cast<unsigned>(size * scale) & ~1u);
}
// Input is the already-upscaled cropped eye. Its texture coordinates use GL's
// bottom-left origin; the shared presenter vertex shader supplies top-left UVs.
inline constexpr const char *kFovBorderFragment = R"GLSL(#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D s;
uniform float fovScale;
uniform int borderMode;
out vec4 c;
void main() {
    vec2 p = (vec2(uv.x, 1.0 - uv.y) - 0.5) / fovScale + 0.5;
    vec2 texel = 1.0 / vec2(textureSize(s, 0));
    vec2 lo = texel * 0.5, hi = 1.0 - lo;
    vec2 q = clamp(p, lo, hi);
    c = texture(s, q);
    // The actual scene is never blurred. Only pixels beyond the cropped FOV
    // get a blur that grows smoothly with their distance from the boundary.
    if (borderMode == 2 && (any(lessThan(p, vec2(0))) || any(greaterThan(p, vec2(1))))) {
        float amount = min(length(p - clamp(p, 0.0, 1.0)) * 0.5, 0.025);
        vec4 sum = c * 4.0;
        sum += texture(s, clamp(q + vec2(amount, 0), lo, hi));
        sum += texture(s, clamp(q - vec2(amount, 0), lo, hi));
        sum += texture(s, clamp(q + vec2(0, amount), lo, hi));
        sum += texture(s, clamp(q - vec2(0, amount), lo, hi));
        c = sum / 8.0;
    }
}
)GLSL";
}
