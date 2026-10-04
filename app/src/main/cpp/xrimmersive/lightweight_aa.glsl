// Bounded directional AA, fused with presentation. No image intermediate or
// endpoint search. RGB luminance retains red/blue edges; alpha is untouched.
// Address math stays highp (mediump UVs caused visible grids on Adreno).
// The specialized central variant receives physical coordinates and needs
// no tap clamping; the CPU has checked the whole filter footprint.
vec4 aaRead(highp vec2 coord, highp vec2 lo, highp vec2 hi) {
    if(centralSampling) return textureLod(ps0,coord,0.0);
    return sceneRead(clamp(coord,lo,hi));
}
float aaLuma(vec3 c) {
    return dot(xrParams.flags.y>0.0 ? c.bgr : c, vec3(0.299,0.587,0.114));
}
vec4 lightweightAA(highp vec2 pos, vec4 original, vec4 reconstructed) {
    // Full strength through radius 0.45; fade ends at 0.47.
    highp vec2 radial=((pos*xrParams.viewport.zw-xrParams.bounds.xy)/
        (xrParams.bounds.zw-xrParams.bounds.xy+1.0))*2.0-1.0;
    highp float radiusSquared=dot(radial,radial);
    if(radiusSquared>=0.2209) return reconstructed;
    float weight=1.0-smoothstep(0.2025,0.2209,radiusSquared);
    highp vec2 lo=(xrParams.bounds.xy+0.5)*xrParams.viewport.xy;
    highp vec2 hi=(xrParams.bounds.zw+0.5)*xrParams.viewport.xy;
    highp vec2 stepUv=xrParams.viewport.xy;
    if(centralSampling) { pos=centralUv(pos); stepUv*=xrParams.packing.zw; }
    float nw=aaLuma(aaRead(pos+vec2(-1,-1)*stepUv,lo,hi).rgb);
    float ne=aaLuma(aaRead(pos+vec2( 1,-1)*stepUv,lo,hi).rgb);
    float sw=aaLuma(aaRead(pos+vec2(-1, 1)*stepUv,lo,hi).rgb);
    float se=aaLuma(aaRead(pos+vec2( 1, 1)*stepUv,lo,hi).rgb);
    float m=aaLuma(original.rgb);
    float low=min(m,min(min(nw,ne),min(sw,se)));
    float high=max(m,max(max(nw,ne),max(sw,se)));
    float contrast=high-low;
    if(contrast<max(0.04,high*0.166)) return reconstructed;
    // Follow the local edge tangent; limit support to two source pixels.
    vec2 tangent=vec2((sw+se)-(nw+ne),(nw+sw)-(ne+se));
    float reduce=max((nw+ne+sw+se)*0.03125,0.0078125);
    highp vec2 aaOffset=clamp(vec2(tangent)/(min(abs(tangent.x),abs(tangent.y))+reduce),
                             vec2(-2),vec2(2))*stepUv/3.0;
    vec3 filtered=0.5*(aaRead(pos-aaOffset,lo,hi).rgb+
                       aaRead(pos+aaOffset,lo,hi).rgb);
    // Replace reconstruction on strong edges instead of sharpening them again.
    // Smooth confidence avoids a visible threshold seam. SGSR remains intact
    // on low-contrast detail; both paths reuse the original center sample.
    float confidence=smoothstep(max(0.04,high*0.166),max(0.08,high*0.332),contrast);
    return vec4(mix(reconstructed.rgb,filtered,weight*confidence),original.a);
}
