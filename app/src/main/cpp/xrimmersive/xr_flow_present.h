#pragma once
#include <GLES3/gl3.h>
// Shared GLES synthesis, evaluated immediately before bilinear/SGSR presentation.
namespace xrimmersive::windowsvr {
inline constexpr const char* kFlowPresentation = R"glsl(
uniform bool fgEnabled;
uniform highp sampler2D fgOld, fgNew, fgFlow;
uniform highp vec2 fgLuma;
highp vec2 fgA,fgB;
highp float fgConfidence;
vec4 fgCenter;
highp vec4 fgMotion(highp vec2 uv) {
    return textureLod(fgFlow,uv*fgLuma/(8.0*vec2(textureSize(fgFlow,0))),0.0);
}
void fgPrepare(highp vec2 uv) {
    if(!fgEnabled) return;
    highp vec4 motion=fgMotion(uv);
    fgA=-0.5*motion.xy/fgLuma; fgB=-0.5*motion.zw/fgLuma;
    highp vec2 a=uv+fgA,b=uv+fgB;
    highp vec2 f=fgMotion(a).xy,r=fgMotion(b).zw;
    vec4 ca=textureLod(fgOld,a,0.0),cb=textureLod(fgNew,b,0.0);
    highp vec3 delta=abs(ca.rgb-cb.rgb);
    fgConfidence=(1.0-smoothstep(1.0,3.0,length(f+r)))*
        (1.0-smoothstep(.08,.25,max(max(delta.r,delta.g),delta.b)));
    if(any(lessThan(min(a,b),vec2(0.0))) || any(greaterThan(max(a,b),vec2(1.0)))) fgConfidence=0.0;
    fgCenter=mix(textureLod(fgNew,uv,0.0),(ca+cb)*.5,fgConfidence);
}
vec4 fgSample(highp vec2 uv) {
    vec4 real=textureLod(fgNew,uv,0.0);
    if(!fgEnabled || fgConfidence<=0.0) return real;
    vec4 generated=(textureLod(fgOld,uv+fgA,0.0)+textureLod(fgNew,uv+fgB,0.0))*.5;
    return mix(real,generated,fgConfidence);
}
)glsl";
inline void bindFlowPresentation(GLuint program,bool enabled,GLuint oldColor,GLuint newColor,GLuint flow,float width,float height) {
    glUniform1i(glGetUniformLocation(program,"fgEnabled"),enabled);
    if(!enabled) return;
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,oldColor);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,flow);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,newColor);
    glUniform1i(glGetUniformLocation(program,"fgOld"),1);
    glUniform1i(glGetUniformLocation(program,"fgNew"),0);
    glUniform1i(glGetUniformLocation(program,"fgFlow"),2);
    glUniform2f(glGetUniformLocation(program,"fgLuma"),width,height);
}
}
