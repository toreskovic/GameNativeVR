#version 450
layout(location=0) out vec4 color;
void main() {
    // Large asymmetric blocks reveal layout/stride errors; fine blue stripes
    // reveal whether reduced-density rendering actually took place.
    color = vec4((floor(gl_FragCoord.x / 64.0) + 1.0) / 8.0,
                 (floor(gl_FragCoord.y / 64.0) + 1.0) / 8.0,
                 mod(floor(gl_FragCoord.x), 2.0), 1.0);
}
