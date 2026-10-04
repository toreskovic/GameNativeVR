#pragma once
namespace xrimmersive::windowsvr {
// History is stored with logical top at v=0, matching the Windows eye textures.
inline constexpr const char *kLsfgCaptureVertex=R"(#version 300 es
out vec2 uv;
void main() {
 vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));
 uv=p; gl_Position=vec4(p*2.0-1.0,0,1);
})";
inline constexpr const char *kLsfgCaptureFragment=R"(#version 300 es
precision highp float;
in vec2 uv;
layout(location=0) out vec4 color;
layout(location=1) out vec4 realColor;
uniform int captureReal;
uniform sampler2D source;
uniform vec4 crop;
uniform mat3 rotation;
uniform vec4 sourceFov;
uniform vec4 targetFov;
uniform int warp;
void main() {
 vec2 p=uv;
 if(warp!=0) {
   vec3 ray=rotation*vec3(mix(targetFov.x,targetFov.y,uv.x),mix(targetFov.z,targetFov.w,uv.y),-1.0);
   vec2 t=ray.xy/max(-ray.z,0.0001);
   p=vec2((t.x-sourceFov.x)/(sourceFov.y-sourceFov.x),(t.y-sourceFov.z)/(sourceFov.w-sourceFov.z));
 }
 // Color-only reprojection cannot reveal hidden pixels; clamp the uncovered edge.
 // Clamp to eye texel centers, not the whole guest texture: packed stereo
 // must never filter a neighboring eye/padding texel into the warped border.
 vec2 halfTexel=0.5/vec2(textureSize(source,0));
 vec2 lo=min(crop.xy,crop.xy+crop.zw)+halfTexel;
 vec2 hi=max(crop.xy,crop.xy+crop.zw)-halfTexel;
 color=texture(source,clamp(crop.xy+p*crop.zw,lo,hi));
 realColor=captureReal!=0 ? texture(source,clamp(crop.xy+uv*crop.zw,lo,hi)) : vec4(0);
})";
}
