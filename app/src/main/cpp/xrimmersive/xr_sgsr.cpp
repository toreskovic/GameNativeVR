#include "xr_sgsr.h"
#include "xr_sgsr_shader.h"
#include <string>
#include <algorithm>
#include <cmath>
#ifdef __ANDROID__
#include <android/log.h>
#define SGSR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#else
#include <cstdio>
#define SGSR_LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#endif

namespace xrimmersive::windowsvr {
bool SgsrUpscaler::initialize(bool edgeDirection, float sharpness) {
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor < 1)) {
        SGSR_LOG("VR SGSR requires GLES 3.1; using bilinear (GLES %d.%d)", major, minor);
        return false;
    }
    GLuint shaders[] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
    // Select at compile time so the basic mode retains its original shader cost.
    std::string fragmentSource = kSgsrFragment;
    if (edgeDirection)
        fragmentSource.insert(fragmentSource.find('\n') + 1, "#define UseEdgeDirection\n");
    const char *sources[] = {kSgsrVertex, fragmentSource.c_str()};
    bool valid = true;
    for (int i = 0; i < 2; ++i) {
        glShaderSource(shaders[i], 1, &sources[i], nullptr);
        glCompileShader(shaders[i]);
        GLint ok = GL_FALSE;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048]{};
            glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            SGSR_LOG("VR SGSR shader failed: %s", log);
            valid = false;
        }
    }
    if (valid) {
        program_ = glCreateProgram();
        for (GLuint shader : shaders) glAttachShader(program_, shader);
        glLinkProgram(program_);
        GLint ok = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048]{};
            glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
            SGSR_LOG("VR SGSR link failed: %s", log);
            shutdown();
        }
    }
    for (GLuint shader : shaders) glDeleteShader(shader);
    if (ready()) {
        glUseProgram(program_);
        const float safeSharpness = std::isfinite(sharpness) ? std::clamp(sharpness, 0.5f, 2.0f) : 0.7f;
        glUniform1f(glGetUniformLocation(program_, "sgsrSharpness"), safeSharpness);
        glUniform1i(glGetUniformLocation(program_, "ps0"), 0);
        viewport_ = glGetUniformLocation(program_, "ViewportInfo[0]");
        bounds_ = glGetUniformLocation(program_, "eyeBounds");
        transform_ = glGetUniformLocation(program_, "sourceTransform");
        fullTexture_ = glGetUniformLocation(program_, "fullTexture");
        glUseProgram(0);
    }
    SGSR_LOG("VR SGSR %s: %s", edgeDirection ? "edge direction" : "basic",
             ready() ? "ready (single pass)" : "unavailable; using bilinear");
    return ready();
}

void SgsrUpscaler::draw(int textureWidth, int textureHeight, int x, int y,
                        int width, int height, bool flipY) {
    const float invW = 1.0f / textureWidth, invH = 1.0f / textureHeight;
    glUseProgram(program_);
    glUniform4f(viewport_, invW, invH, textureWidth, textureHeight);
    glUniform4i(bounds_, x, y, x + width - 1, y + height - 1);
    glUniform1i(fullTexture_, x == 0 && y == 0 && width == textureWidth && height == textureHeight);
    glUniform4f(transform_, x * invW, (flipY ? y + height : y) * invH,
                width * invW, (flipY ? -height : height) * invH);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void SgsrUpscaler::shutdown() {
    if (program_) glDeleteProgram(program_);
    program_ = 0;
}
}
