// Offscreen image tests for the production VR SGSR shader. No OpenXR runtime needed.
#include "xr_fov.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>

static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (GL=%x EGL=%x)\n", message, glGetError(), eglGetError()); std::exit(1); }
}
int main() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    require(eglInitialize(display, nullptr, nullptr), "EGL initialize");
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count;
    require(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1, "EGL config");
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint surfAttrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfAttrs);
    require(eglMakeCurrent(display, surface, surface, context), "EGL current");
    std::printf("GLES: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const float vertices[] = {-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0};
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), reinterpret_cast<void *>(2*sizeof(float)));
    using namespace xrimmersive::windowsvr;
    for (float scale : {0.7f, 0.8f, 0.85f, 0.99f, 1.0f}) {
        for (auto angles : {std::array<float,2>{-0.7f, 0.95f}, {-0.95f, 0.7f}, {0.9f, -0.8f}}) {
            float a = angles[0], b = angles[1];
            const float originalSpan = std::tan(b) - std::tan(a);
            const float center = (std::tan(a) + std::tan(b)) / 2;
            scaleFovPair(a, b, scale);
            require(std::abs((std::tan(b)-std::tan(a))/originalSpan-scale)<1e-6, "tangent span scales");
            require(std::abs((std::tan(a)+std::tan(b))/2-center)<1e-6, "asymmetric eye midpoint preserved");
            scaleFovPair(a, b, 1/scale);
            require(std::abs(a-angles[0])<1e-6 && std::abs(b-angles[1])<1e-6, "full FOV reconstructed");
        }
    }
    require(scaledFovDimension(1964, .8f)==1570 && scaledFovDimension(2160, .8f)==1728, "even reduced dimensions");
    const char *vertex = "#version 300 es\nlayout(location=0) in vec2 p;layout(location=1) in vec2 t;out vec2 uv;void main(){uv=t;gl_Position=vec4(p,0,1);}";
    GLuint program=glCreateProgram();
    const char *sources[]={vertex,kFovBorderFragment};
    for (int i=0;i<2;++i) {
        GLuint shader=glCreateShader(i==0?GL_VERTEX_SHADER:GL_FRAGMENT_SHADER);
        glShaderSource(shader,1,&sources[i],nullptr);glCompileShader(shader);
        GLint ok;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(shader,sizeof(log),nullptr,log); std::puts(log); }
        require(ok,"production border shader compile");
        glAttachShader(program,shader);glDeleteShader(shader);
    }
    glLinkProgram(program);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);require(ok,"border link");
    glUseProgram(program);glUniform1i(glGetUniformLocation(program,"s"),0);
    GLuint input;glGenTextures(1,&input);glBindTexture(GL_TEXTURE_2D,input);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    std::array<unsigned char,32*32*4> pixels{};
    std::array<unsigned char,64*64*4> result{},extended{};
    for(int y=0;y<32;++y)for(int x=0;x<32;++x){
        int i=(y*32+x)*4;pixels[i]=x*8;pixels[i+1]=y*8;pixels[i+2]=(y%2)?255:0;pixels[i+3]=255;
    }
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    glViewport(0,0,64,64);
    for(float scale:{.7f,.8f,1.f})for(int mode:{1,2}){
        glUniform1f(glGetUniformLocation(program,"fovScale"),scale);
        glUniform1i(glGetUniformLocation(program,"borderMode"),mode);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,result.data());
        require(glGetError()==GL_NO_ERROR,"border draw");
        int changed=0;
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){
            float u=((x+.5f)/64-.5f)/scale+.5f, v=((y+.5f)/64-.5f)/scale+.5f;
            const bool inside=u>=0&&u<=1&&v>=0&&v<=1;
            const int i=(y*64+x)*4;
            if(mode==1||inside){
                require(std::abs(result[i]-std::clamp((u*32-.5f)*8,0.f,248.f))<=2,"horizontal extent and edge extension");
                require(std::abs(result[i+1]-std::clamp((v*32-.5f)*8,0.f,248.f))<=2,"vertical orientation and edge extension");
            }
            if(mode==2){
                if(inside)for(int c=0;c<4;++c)require(result[i+c]==extended[i+c],"blur leaves scene unchanged");
                if(!inside&&result[i+2]!=extended[i+2])++changed;
            }
        }
        if(mode==1)extended=result;
        if(mode==2&&scale<1)require(changed>0,"blur modifies border detail");
    }
    glDeleteTextures(1,&input);glDeleteProgram(program);
    glDeleteBuffers(1,&vbo);glDeleteVertexArrays(1,&vao);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);
    std::puts("FOV projection and border image tests passed");
}
