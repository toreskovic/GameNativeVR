// Offscreen image tests for the production VR SGSR shader. No OpenXR runtime needed.
#include "xr_sgsr.h"
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
int main(int argc, char **argv) {
    const bool edgeDirection = argc > 1 && std::string(argv[1]) == "--edge";
    const float sharpness = argc > 2 ? std::strtof(argv[2], nullptr) : 0.7f;
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
    xrimmersive::windowsvr::SgsrUpscaler sgsr;
    require(sgsr.initialize(edgeDirection, sharpness), "SGSR initialization");
    GLuint input, output, fbo;
    glGenTextures(1, &input); glBindTexture(GL_TEXTURE_2D, input);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "output framebuffer");
    glViewport(0, 0, 64, 64);
    std::array<unsigned char, 32*32*4> pixels{};
    std::array<unsigned char, 64*64*4> result{};
    auto upload = [&] {
        glBindTexture(GL_TEXTURE_2D, input);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    };
    auto read = [&] {
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        require(glGetError() == GL_NO_ERROR, "render without GL errors");
    };
    // Constant colors (including black/white): catch NaNs, clipping and alpha loss.
    for (unsigned char value : {0, 77, 255}) {
        for (int i=0; i<32*32; ++i) {
            for (int c=0; c<3; ++c) pixels[i*4+c]=value;
            pixels[i*4+3]=123;
        }
        upload(); sgsr.draw(32, 32, 0, 0, 32, 32, false); read();
        for (int i=0; i<64*64; ++i) {
            for (int c=0; c<3; ++c) require(std::abs(int(result[i*4+c])-value)<=1, "constant color");
            require(std::abs(int(result[i*4+3])-123)<=1, "alpha");
        }
    }
    // Axis-aligned ramps check pixel-center alignment through the shader.
    for (bool vertical : {false, true}) {
        for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
            int i=(y*32+x)*4;
            pixels[i]=pixels[i+1]=pixels[i+2]=(vertical?y:x)*8; pixels[i+3]=255;
        }
        upload(); sgsr.draw(32, 32, 0, 0, 32, 32, false); read();
        for (int y=8; y<56; ++y) for (int x=8; x<56; ++x) {
            int expected=(vertical?63-y:x)*4-2;
            require(std::abs(int(result[(y*64+x)*4])-expected)<=2, "pixel-center alignment");
        }
    }
    // Diagonal edges exercise the directional filter (including low contrast).
    unsigned long long diagonalHash = 1469598103934665603ULL;
    for (int contrast : {4, 128}) {
        for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
            const int i=(y*32+x)*4;
            pixels[i]=pixels[i+1]=pixels[i+2]=64 + (x > y ? contrast : 0);
            pixels[i+3]=255;
        }
        upload(); sgsr.draw(32, 32, 0, 0, 32, 32, false); read();
        for (int i=0; i<64*64; ++i) {
            require(result[i*4] == result[i*4+1] && result[i*4] == result[i*4+2], "neutral diagonal edge");
            require(result[i*4] >= 41 && result[i*4] <= 64+contrast+23, "bounded diagonal edge");
            diagonalHash = (diagonalHash ^ result[i*4]) * 1099511628211ULL;
        }
    }
    std::printf("Diagonal image hash: %llx\n", diagonalHash);
    // Packed eyes: right half pure blue must never leak into cropped left eye.
    // Within the left eye the top is red, bottom green, to verify both Y modes.
    for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
        int i=(y*32+x)*4;
        pixels[i]=x<16 && y<16 ? 255:0;
        pixels[i+1]=x<16 && y>=16 ? 255:0;
        pixels[i+2]=x>=16 ? 255:0;
        pixels[i+3]=255;
    }
    upload();
    for (bool flip : {false, true}) {
        sgsr.draw(32, 32, 0, 0, 16, 32, flip); read();
        for (int i=0; i<64*64; ++i) require(result[i*4+2]<=23, "no adjacent eye bleed");
        require(result[(8*64+32)*4+(flip?0:1)]>250, "bottom orientation");
        require(result[(55*64+32)*4+(flip?1:0)]>250, "top orientation");
    }
    // Changing the adjacent eye must not affect any output pixel of this eye.
    sgsr.draw(32, 32, 0, 0, 16, 32, false); read();
    const auto leftEye = result;
    for (int y=0; y<32; ++y) for (int x=16; x<32; ++x) {
        const int i=(y*32+x)*4;
        pixels[i]=pixels[i+1]=pixels[i+2]=255;
    }
    upload(); sgsr.draw(32, 32, 0, 0, 16, 32, false); read();
    require(result == leftEye, "adjacent eye independence");
    // Restore the blue right eye.
    for (int y=0; y<32; ++y) for (int x=16; x<32; ++x) {
        const int i=(y*32+x)*4; pixels[i]=pixels[i+1]=0;
    }
    upload();
    // Nonzero crop offset: right eye must remain blue, including every edge tap.
    sgsr.draw(32, 32, 16, 0, 16, 32, false); read();
    for (int i=0; i<64*64; ++i) require(result[i*4]==0 && result[i*4+1]==0 && result[i*4+2]==255, "offset crop");
    // Fixed foveated upscaling: peripheral pixels must equal bilinear for
    // either eye crop and either vertical orientation; centre still uses SGSR.
    for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
        int i=(y*32+x)*4;
        pixels[i]=pixels[i+1]=pixels[i+2]=((x/3+y/3)%2) ? 192 : 64;
        pixels[i+3]=123;
    }
    upload();
    for (int cropWidth : {16, 32}) for (int cropX : {0, 16}) for (bool flip : {false, true}) {
        if (cropX + cropWidth > 32) continue;
        sgsr.draw(32, 32, cropX, 0, cropWidth, 32, flip); read();
        int changedCentre = 0, testedEdges = 0;
        for (int y=0; y<64; ++y) for (int x=0; x<64; ++x) {
            float u=(x+.5f)/64, v=(y+.5f)/64;
            if (!flip) v=1-v;
            float px=cropX+u*cropWidth-.5f, py=v*32-.5f;
            int ix=int(std::floor(px)), iy=int(std::floor(py));
            float fx=px-ix, fy=py-iy;
            auto texel = [&](int tx, int ty) {
                tx=std::max(cropX,std::min(cropX+cropWidth-1,tx));
                ty=std::max(0,std::min(31,ty));
                return float(pixels[(ty*32+tx)*4]);
            };
            float bilinear=(1-fy)*((1-fx)*texel(ix,iy)+fx*texel(ix+1,iy))+
                fy*((1-fx)*texel(ix,iy+1)+fx*texel(ix+1,iy+1));
            float r2=(2*u-1)*(2*u-1)+(2*v-1)*(2*v-1);
            int delta=std::abs(int(result[(y*64+x)*4])-int(std::round(bilinear)));
            if (r2 >= .510f) {
                require(delta<=2,"foveated periphery equals bilinear");
                ++testedEdges;
            }
            if (r2 < .483f && delta>2) ++changedCentre;
            require(std::abs(int(result[(y*64+x)*4+3])-123)<=1,"foveated alpha");
        }
        require(testedEdges>2400 && testedEdges<2520,"60-percent bilinear coverage");
        require(changedCentre>0,"SGSR remains active in centre");
    }
    sgsr.shutdown();
    // A half-strength result must be halfway between bilinear and base SGSR,
    // including RGB channels which saturate before blending.
    for (int i=0; i<32*32; ++i) { pixels[i*4]=255; pixels[i*4+2]=0; }
    upload();
    require(sgsr.initialize(edgeDirection, 1.0f), "base blend reference");
    sgsr.draw(32,32,0,0,32,32,true); read();
    auto full = result;
    sgsr.shutdown();
    require(sgsr.initialize(edgeDirection, 0.5f), "half blend");
    sgsr.draw(32,32,0,0,32,32,true); read();
    for (int y=0; y<64; ++y) for (int x=0; x<64; ++x) {
        float px=(x+.5f)*.5f-.5f, py=(y+.5f)*.5f-.5f;
        int ix=int(std::floor(px)), iy=int(std::floor(py));
        float fx=px-ix, fy=py-iy;
        for (int c=0; c<3; ++c) {
            auto sample = [&](int tx,int ty) {
                tx=std::clamp(tx,0,31); ty=std::clamp(ty,0,31);
                return float(pixels[(ty*32+tx)*4+c]);
            };
            float bilinear=(1-fy)*((1-fx)*sample(ix,iy)+fx*sample(ix+1,iy))+
                fy*((1-fx)*sample(ix,iy+1)+fx*sample(ix+1,iy+1));
            int i=(y*64+x)*4+c;
            require(std::abs(result[i] - (bilinear+full[i])*.5f)<=2.0f, "half-strength RGB blend");
        }
        require(result[(y*64+x)*4+3]==123, "blend preserves alpha");
    }
    sgsr.shutdown();
    // Recreating resources must also work (new XR session).
    require(sgsr.initialize(edgeDirection, sharpness), "reinitialize"); sgsr.shutdown();
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &input); glDeleteTextures(1, &output);
    glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    std::puts("PASS: SGSR shader compilation, constant colors, alpha, pixel centers, eye crops, Y flips, resource recreation");
}
