#pragma once
#include "../../app/src/main/cpp/xrimmersive/xr_flow_present.h"
#include "../../app/src/main/cpp/xrimmersive/xr_sgsr.h"
#include "../../app/src/main/cpp/xrimmersive/xr_sgsr_shader.h"
#include <EGL/egl.h>
#include <vector>
#include <string>
#include <cassert>
// Render the production GLES synthesis from actual Vulkan-generated flow readback.
// This intentionally exercises shader contents, not Android AHB interop.
struct PresentationTest {
    EGLDisplay display; EGLContext context; EGLSurface surface;
    GLuint program,vao,vbo;
    int mode;
    static GLuint shader(GLenum type,const char* text) {
        GLuint s=glCreateShader(type); glShaderSource(s,1,&text,nullptr); glCompileShader(s);
        GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok) {char msg[4096];glGetShaderInfoLog(s,sizeof(msg),nullptr,msg);std::cerr<<msg;std::abort();} return s;
    }
    explicit PresentationTest(int m):mode(m) {
        using namespace xrimmersive::windowsvr;
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY); assert(eglInitialize(display,nullptr,nullptr));
        const EGLint cfg[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_NONE};
        EGLConfig config; EGLint n;assert(eglChooseConfig(display,cfg,&config,1,&n)&&n);
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},sa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);surface=eglCreatePbufferSurface(display,config,sa);
        assert(eglMakeCurrent(display,surface,surface,context));
        std::string fs="#version 310 es\nprecision highp float;layout(location=0) in vec2 uv;layout(location=0) out vec4 c;\n";
        fs+=kFlowPresentation; fs+="void main(){fgPrepare(uv);c=fgCenter;}";
        auto vs=shader(GL_VERTEX_SHADER,kSgsrVertex),frag=shader(GL_FRAGMENT_SHADER,fs.c_str());
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,frag);glLinkProgram(program);
        GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);glDeleteShader(vs);glDeleteShader(frag);
        glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        const float vertices[]={-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1};
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,nullptr);
        glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,reinterpret_cast<void*>(8));
        assert(mode==1);
    }
    std::vector<unsigned char> render(int w,int h,int fw,int fh,const unsigned char* old,const unsigned char* current,const void* packed) {
        GLuint tex[4],fbo;glGenTextures(4,tex);
        for(int i=0;i<4;++i) {
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,0,i==2?GL_RGBA16F:GL_RGBA8,i==2?(fw+7)/8:w,i==2?(fh+7)/8:h,0,GL_RGBA,
                i==2?GL_HALF_FLOAT:GL_UNSIGNED_BYTE,i==0?old:i==1?current:i==2?packed:nullptr);
        }
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex[3],0);
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);glViewport(0,0,w,h);
        {
            glUseProgram(program);glUniform4f(glGetUniformLocation(program,"sourceTransform"),0,0,1,1);
            xrimmersive::windowsvr::bindFlowPresentation(program,true,tex[0],tex[1],tex[2],fw,fh);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        }
        std::vector<unsigned char> pixels(w*h*4);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        assert(glGetError()==GL_NO_ERROR);glDeleteFramebuffers(1,&fbo);glDeleteTextures(4,tex);return pixels;
    }
    ~PresentationTest() {
        glDeleteProgram(program);glDeleteBuffers(1,&vbo);glDeleteVertexArrays(1,&vao);
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);
    }
};
