// Shared by luminance and synthesis: flow lives in anchor UV coordinates.
layout(push_constant) uniform Params {
    ivec4 size;
    vec4 oldTransform[3];
    vec4 newTransform[3];
} p;
vec2 rawUv(vec2 uv, bool current) {
    // Match clamp-to-edge sampling of the former aligned texture. Coordinates
    // inside the image are transformed before the raw sampler clamps its edges.
    uv=clamp(uv,vec2(0.0),vec2(1.0));
    mat3 m=current ? mat3(p.newTransform[0].xyz,p.newTransform[1].xyz,p.newTransform[2].xyz)
                   : mat3(p.oldTransform[0].xyz,p.oldTransform[1].xyz,p.oldTransform[2].xyz);
    vec3 q=m*vec3(uv,1.0);
    return q.xy/max(q.z,0.0001);
}
