#version 450
layout(location=0) out vec2 uv;
layout(push_constant) uniform Parameters { vec4 logicalCuts; vec4 packedCuts; } p;
void main() {
    // Nine rectangles, 18 triangles, one draw/pass. Integer texel-aligned cuts
    // are computed once on the CPU; the rasterizer interpolates source UVs.
    const ivec2 corners[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(1,1),
                                  ivec2(0,0),ivec2(1,1),ivec2(0,1));
    int region=gl_VertexIndex/6;
    ivec2 point=ivec2(region%3,region/3)+corners[gl_VertexIndex%6];
    vec4 sx=vec4(0,p.logicalCuts.xy,1), sy=vec4(0,p.logicalCuts.zw,1);
    vec4 dx=vec4(0,p.packedCuts.xy,1), dy=vec4(0,p.packedCuts.zw,1);
    uv=vec2(sx[point.x],sy[point.y]);
    gl_Position=vec4(vec2(dx[point.x],dy[point.y])*2.0-1.0,0,1);
}
