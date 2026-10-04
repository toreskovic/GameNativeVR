#!/usr/bin/env python3
"""Port the existing licensed SGSR shader; preserve a single source of its math."""
from pathlib import Path
import re, subprocess, tempfile, struct
root=Path(__file__).resolve().parents[2]/'app/src/main/cpp/xrimmersive'
s=(root/'xr_sgsr_shader.h').read_text().split('kSgsrFragment = R"glsl(')[1].split(')glsl"')[0]
s=s.replace('#version 310 es','#version 450').replace('uniform float sgsrSharpness;','#define sgsrSharpness xrParams.control.x')
a=s.index('#if defined(UseUniformBlock)');b=s.index('float fastLanczos2',a)
s=s[:a]+'''layout(set=0,binding=0) uniform sampler2D ps0;
layout(set=0,binding=1) uniform sampler2D overlayTexture;
// SGSR keeps mediump color/filter arithmetic, but image addressing must remain
// highp through every stage, including bilinear-only and FOV border paths.
layout(constant_id=0) const bool fxaaEnabled=true;
layout(constant_id=1) const bool packedInput=false;
layout(constant_id=2) const bool centralSampling=false;
layout(location=0) in highp vec2 screenUv;
layout(location=0) out vec4 out_Target0;
layout(push_constant) uniform Parameters { highp vec4 crop, viewport, bounds, control, flags, content, fxaaPatch, packing; } xrParams;
#define eyeBounds ivec4(xrParams.bounds)
#define fullTexture (xrParams.crop==vec4(0,0,1,1))
highp vec2 uv;
// Packing v1 keeps logical coordinates throughout SGSR/AA; only texture
// accesses are remapped. No full-resolution unpack intermediate is created.
highp vec2 transportUv(highp vec2 coord) {
    return (0.5*coord+0.5*clamp(coord-xrParams.packing.xy,vec2(0),
        1.0-2.0*xrParams.packing.xy))*xrParams.packing.zw;
}
highp vec2 centralUv(highp vec2 coord) {
    return (coord-0.5*xrParams.packing.xy)*xrParams.packing.zw;
}
vec4 sceneRead(highp vec2 coord) {
    return textureLod(ps0,packedInput?transportUv(coord):coord,0.0);
}
vec4 sceneFetch(highp ivec2 pixel) {
    if(packedInput) return sceneRead((vec2(pixel)+0.5)*xrParams.viewport.xy);
    return texelFetch(ps0,pixel,0);
}
vec4 sceneGather(highp vec2 coord) {
    if(!packedInput) return textureGather(ps0,coord,1);
    // In the full-rate center, an integral texel translation preserves native
    // gather exactly. Other extents/boundaries need logical reconstruction.
    highp vec2 low=xrParams.packing.xy+xrParams.viewport.xy;
    if(all(greaterThan(coord,low)) && all(lessThan(coord,1.0-low)))
        return textureGather(ps0,transportUv(coord),1);
    // Hardware gather addresses physical neighbors. Reconstruct the four
    // logical neighbors instead, including taps crossing a density boundary.
    highp ivec2 pixel=ivec2(floor(coord*xrParams.viewport.zw-0.5));
    return vec4(sceneFetch(clamp(pixel+ivec2(0,1),eyeBounds.xy,eyeBounds.zw)).g,
                sceneFetch(clamp(pixel+ivec2(1,1),eyeBounds.xy,eyeBounds.zw)).g,
                sceneFetch(clamp(pixel+ivec2(1,0),eyeBounds.xy,eyeBounds.zw)).g,
                sceneFetch(clamp(pixel,eyeBounds.xy,eyeBounds.zw)).g);
}
vec4 aaCenter;
''' +s[b:]
s=s.replace('ViewportInfo[0]','xrParams.viewport').replace('void main()', 'void upscale()')
# Adapt only SGSR reads, not FXAA's original-image reads or the quad UI.
s=s.replace('textureLod(ps0, center, 0.0)', 'aaCenter')
# Keep the helper implementations above untouched.
a=s.index('vec4 gatherEye(')
s=s[:a]+s[a:].replace('textureGather(ps0, coord, 1)', 'sceneGather(coord)')
s=re.sub(r'texelFetch\(ps0, (clamp\([^\n]+\)), 0\)', r'sceneFetch(\1)', s)
# Only SGSR gather coordinates switch to physical space. Its weights and
# fractional source position remain in logical texels.
s=s.replace('vec4 gatherEye(highp vec2 coord) {',
    'vec4 gatherEye(highp vec2 coord) {\n    if(centralSampling) return textureGather(ps0,coord,1);')
s=s.replace('highp vec2 coord = (imgCoordPixel*xrParams.viewport.xy);',
    'highp vec2 coord = (imgCoordPixel*xrParams.viewport.xy);\n'
    '        highp vec2 gatherStep=xrParams.viewport.xy;\n'
    '        if(centralSampling) { coord=centralUv(coord); gatherStep*=xrParams.packing.zw; }')
s=s.replace('coord.x += xrParams.viewport.x;', 'coord.x += gatherStep.x;')
s=s.replace('gatherEye(coord + vec2(xrParams.viewport.x, 0.0))','gatherEye(coord + vec2(gatherStep.x, 0.0))')
s=s.replace('gatherEye(coord + vec2(0.0, -xrParams.viewport.y))','gatherEye(coord + vec2(0.0, -gatherStep.y))')
s=s.replace('gatherEye(coord + vec2(0.0, xrParams.viewport.y))','gatherEye(coord + vec2(0.0, gatherStep.y))')
s+=(root/'lightweight_aa.glsl').read_text()

s+='''
vec4 sampleScene(highp vec2 pos) {
    uv=xrParams.crop.xy+pos*xrParams.crop.zw;
    highp vec2 lo=(xrParams.bounds.xy+0.5)*xrParams.viewport.xy;
    highp vec2 hi=(xrParams.bounds.zw+0.5)*xrParams.viewport.xy;
    highp vec2 center=clamp(uv,lo,hi);
    aaCenter=sceneRead(center);
    vec4 result=aaCenter;
    if(xrParams.flags.x>0.0) { upscale();result=out_Target0; }
    if(fxaaEnabled && xrParams.content.z>0.0) result=lightweightAA(center,aaCenter,result);
    highp vec2 radial=((center*xrParams.viewport.zw-xrParams.bounds.xy)/
        (xrParams.bounds.zw-xrParams.bounds.xy+1.0))*2.0-1.0;
    vec3 tint=vec3(0);
    bool debug=false;
    // Game FFR visualization is rendered by the game layer using FragSizeEXT.
    if(xrParams.fxaaPatch.x==2.0 && (xrParams.flags.x>0.0 ||
              (fxaaEnabled && xrParams.content.z>0.0))) {
        float weight=1.0-smoothstep(0.2025,0.2209,dot(radial,radial));
        tint=mix(vec3(1,0,0),vec3(0,0,1),weight);
        debug=true;
    }
    // Final channel swap applies to the whole result, including the debug tint.
    if(debug) result.rgb=mix(result.rgb,xrParams.flags.y>0.0?tint.bgr:tint,0.35);
    return result;
}
vec3 linearColor(vec3 c) { return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045))); }
void main() {
    vec4 c;
    if(xrParams.flags.z>0.0) {
        highp vec2 q=(screenUv-0.5)/xrParams.content.xy+0.5;
        bool inside=all(greaterThanEqual(q,vec2(0)))&&all(lessThanEqual(q,vec2(1)));
        vec4 game= xrParams.flags.w>0.0&&inside ? textureLod(ps0,q,0.0) : vec4(0,0,0,1);
        vec4 overlay=textureLod(overlayTexture,screenUv,0.0);
        // Bitmap-only fallback and transparent overlay use the same upload.
        c=xrParams.flags.w>0.0 ? vec4(mix(linearColor(game.rgb),linearColor(overlay.rgb),overlay.a),inside?1.0:overlay.a) : vec4(linearColor(overlay.rgb),overlay.a);
        if(xrParams.control.w==0.0) c.rgb=mix(c.rgb*12.92,1.055*pow(c.rgb,vec3(1.0/2.4))-0.055,greaterThan(c.rgb,vec3(0.0031308)));
    } else {
        highp vec2 q=xrParams.control.z>0.0 ? (screenUv-0.5)/xrParams.control.y+0.5 : screenUv;
        highp vec2 halfTexel=0.5/(xrParams.bounds.zw-xrParams.bounds.xy+1.0);
        highp vec2 clamped=clamp(q,halfTexel,1.0-halfTexel);
        c=sampleScene(clamped);
        if(xrParams.control.z>1.5 && (any(lessThan(q,vec2(0)))||any(greaterThan(q,vec2(1))))) {
            highp float a=min(length(q-clamp(q,0.0,1.0))*0.5,0.025);
            c=(c*4.0+sampleScene(clamp(clamped+vec2(a,0),halfTexel,1.0-halfTexel))
              +sampleScene(clamp(clamped-vec2(a,0),halfTexel,1.0-halfTexel))
              +sampleScene(clamp(clamped+vec2(0,a),halfTexel,1.0-halfTexel))
              +sampleScene(clamp(clamped-vec2(0,a),halfTexel,1.0-halfTexel)))/8.0;
        }
        if(xrParams.flags.y>0.0) c=c.bgra;
        // Match the GLES projection path: scene samples go straight to the
        // attachment. An sRGB attachment encodes shader output automatically.
        // Decoding here incorrectly darkens scene values; bitmap UI above has
        // its own, explicitly encoded input contract.
    }
    out_Target0=c;
}
'''
vert='''#version 450
layout(location=0) out highp vec2 screenUv;
void main() { vec2 q=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);screenUv=q;gl_Position=vec4(q*2.0-1.0,0,1); }
'''
maskedVert='''#version 450
layout(location=0) in vec2 position;
layout(location=0) out highp vec2 screenUv;
void main() { screenUv=position;gl_Position=vec4(position*2.0-1.0,0,1); }
'''
header='// Generated by tools/xr-vulkan/build-shaders.py. SGSR: see LICENSE-SGSR.txt. Lightweight AA is project-local.\n#pragma once\n#include <cstdint>\nnamespace xrimmersive {\n'

def check_coordinate_precision(binary):
 # Software Vulkan may execute RelaxedPrecision as FP32, hiding the mobile bug
 # from image comparisons. Check the actual generated SPIR-V as well.
 assembly=subprocess.check_output(['spirv-dis',str(binary)],text=True)
 names=dict(re.findall(r'OpName\s+(%\S+)\s+"([^"]+)"',assembly))
 required={'screenUv','uv','pos','lo','hi','q','halfTexel','clamped','a',
           'center','eyeSize','eyePosition','radial','radiusSquared',
           'imgCoord','imgCoordPixel','coord','aaOffset','gatherStep'}
 assert required <= set(names.values()), 'Missing coordinate precision coverage'
 relaxed=set(re.findall(r'OpDecorate\s+(%\S+)\s+RelaxedPrecision',assembly))
 bad=[name for id,name in names.items() if name in required and id in relaxed]
 assert not bad, f'Reduced-precision image addressing: {bad}'
 parameters=next(id for id,name in names.items() if name=='Parameters')
 assert not re.search(r'OpMemberDecorate\s+'+re.escape(parameters)+r'\s+\d+\s+RelaxedPrecision',assembly), 'Reduced-precision image parameters'
 # Keep the existing reduced-precision SGSR/color math; this is not an FP32
 # conversion of the whole filter.
 assert any(name=='color' and id in relaxed for id,name in names.items())

with tempfile.TemporaryDirectory() as t:
 for name,code,stage in [('presentVertex',vert,'vert'),('presentMaskedVertex',maskedVert,'vert'),('presentBasic',s,'frag'),('presentEdge',s.replace('#version 450','#version 450\n#define UseEdgeDirection'),'frag')]:
  src=Path(t)/(name+'.'+stage);src.write_text(code);out=src.with_suffix('.spv')
  subprocess.run(['glslangValidator','-V','--target-env','vulkan1.1','-o',str(out),str(src)],check=True)
  subprocess.run(['spirv-val','--target-env','vulkan1.1',str(out)],check=True)
  if stage=='frag': check_coordinate_precision(out)
  words=struct.unpack('<'+'I'*(out.stat().st_size//4),out.read_bytes())
  header+=f'inline constexpr uint32_t {name}[]={{\n'+''.join(','.join(hex(x) for x in words[i:i+8])+',\n' for i in range(0,len(words),8))+'};\n'
(root/'xr_vulkan_present_spv.h').write_text(header+'}\n')
