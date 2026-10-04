#version 450
layout(set=0,binding=0) uniform sampler2D source;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    // sRGB inputs use an sRGB attachment view for encoding. The shared AHB
    // remains RGBA8 UNORM for Android, preserving the encoded-byte contract.
    color=textureLod(source,uv,0.0);
}
