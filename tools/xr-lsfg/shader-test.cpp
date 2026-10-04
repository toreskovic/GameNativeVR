#include "../../app/src/main/cpp/xrimmersive/xr_frame_interpolation.h"
#include "../../app/src/main/cpp/xrimmersive/xr_lsfg_shaders.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
using namespace xrimmersive::windowsvr;
void require(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s GL=%x EGL=%x\n", what, glGetError(), eglGetError());
        exit(1);
    }
}
int main() {
    auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    require(eglInitialize(display, nullptr, nullptr), "display");
    EGLint attrs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES3_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_NONE};
    EGLConfig cfg;
    EGLint n;
    require(eglChooseConfig(display, attrs, &cfg, 1, &n) && n, "config");
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}, sa[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    auto ctx = eglCreateContext(display, cfg, EGL_NO_CONTEXT, ca);
    auto surf = eglCreatePbufferSurface(display, cfg, sa);
    require(eglMakeCurrent(display, surf, surf, ctx), "current");
    GLuint p = glCreateProgram();
    const char *sources[] = {kLsfgCaptureVertex, kLsfgCaptureFragment};
    for (int i = 0; i < 2; ++i) {
        GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        glShaderSource(s, 1, &sources[i], nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        require(ok, "shader compile");
        glAttachShader(p, s);
        glDeleteShader(s);
    }
    glLinkProgram(p);
    GLint linked;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    require(linked, "link");
    glUseProgram(p);
    GLuint tex, fbo, out, vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    std::array<unsigned char, 32 * 32 * 4> pixels{}, result{};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            int i = (y * 32 + x) * 4;
            pixels[i] = x * 8;
            pixels[i + 1] = y * 8;
            pixels[i + 2] = 77;
            pixels[i + 3] = 255;
        }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &out);
    glBindTexture(GL_TEXTURE_2D, out);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "fbo");
    glViewport(0, 0, 32, 32);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(p, "source"), 0);
    glUniform4f(glGetUniformLocation(p, "crop"), 0, 0, 1, 1);
    float id[] = {0, 0, 0, 1};
    auto m = targetToSourceRotation(id, id);
    glUniformMatrix3fv(glGetUniformLocation(p, "rotation"), 1, GL_FALSE, m.data());
    glUniform4f(glGetUniformLocation(p, "sourceFov"), -.6f, .9f, 1.f, -.7f);
    glUniform4f(glGetUniformLocation(p, "targetFov"), -.6f, .9f, 1.f, -.7f);
    auto draw = [&] {
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        require(glGetError() == GL_NO_ERROR, "draw");
    };
    for (int warp : {0, 1}) {
        glUniform1i(glGetUniformLocation(p, "warp"), warp);
        draw();
        for (size_t i = 0; i < pixels.size(); ++i)
            require(abs(int(pixels[i]) - result[i]) <= 1, "identity/asymmetric FOV");
    }
    glUniform1i(glGetUniformLocation(p, "warp"), 0);
    glUniform4f(glGetUniformLocation(p, "crop"), 0, 1, 1, -1);
    draw();
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            require(abs(int(result[(y * 32 + x) * 4 + 1]) - (31 - y) * 8) <= 1, "Y flip");
    glUniform4f(glGetUniformLocation(p, "crop"), .25f, .25f, .5f, .5f);
    draw();
    for (int y = 1; y < 31; ++y)
        for (int x = 1; x < 31; ++x) {
            int i = (y * 32 + x) * 4;
            require(abs(int(result[i]) - (62 + 4 * x)) <= 1, "packed-eye crop X");
            require(abs(int(result[i + 1]) - (62 + 4 * y)) <= 1, "packed-eye crop Y");
        }
    glUniform4f(glGetUniformLocation(p, "crop"), 0, 0, 1, 1);
    glUniform1i(glGetUniformLocation(p, "warp"), 1);
    glUniform4f(glGetUniformLocation(p, "sourceFov"), -1, 1, 1, -1);
    glUniform4f(glGetUniformLocation(p, "targetFov"), -1, 1, 1, -1);
    float yaw[] = {0, std::sin(.05f), 0, std::cos(.05f)};
    m = targetToSourceRotation(id, yaw);
    glUniformMatrix3fv(glGetUniformLocation(p, "rotation"), 1, GL_FALSE, m.data());
    draw();
    int x = 16, y = 16;
    float u = (x + .5f) / 32, v = (y + .5f) / 32;
    float ray[] = {2 * u - 1, 1 - 2 * v, -1}, r[3]{};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            r[a] += m[b * 3 + a] * ray[b];
    float expectedU = (r[0] / -r[2] + 1) * .5f, expectedV = (1 - r[1] / -r[2]) * .5f;
    require(abs(int(result[(y * 32 + x) * 4]) - int(expectedU * 256 - 4)) <= 2, "yaw compensation X");
    require(abs(int(result[(y * 32 + x) * 4 + 1]) - int(expectedV * 256 - 4)) <= 2, "yaw compensation Y");
    // Fused guest crop + Y flip + rotation must map the same logical eye ray.
    glUniform4f(glGetUniformLocation(p, "crop"), .25f, .75f, .5f, -.5f);
    draw();
    require(abs(int(result[(y * 32 + x) * 4]) - int((.25f + .5f * expectedU) * 256 - 4)) <= 2,
            "fused crop/rotation X");
    require(abs(int(result[(y * 32 + x) * 4 + 1]) - int((.75f - .5f * expectedV) * 256 - 4)) <= 2,
            "fused crop/flip/rotation Y");
    require(abs(int(result[(16 * 32) * 4]) - 64) <= 1, "warped border stays inside packed eye");
    // MRT must produce the same aligned input while independently preserving
    // an unwarped cropped/flipped real endpoint for native presentation.
    const auto aligned = result;
    GLuint real;
    glGenTextures(1, &real);
    glBindTexture(GL_TEXTURE_2D, real);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, real, 0);
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachments);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "MRT fbo");
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(p, "captureReal"), 1);
    draw();
    require(result == aligned, "MRT aligned output unchanged");
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            int i = (y * 32 + x) * 4;
            require(abs(int(result[i]) - std::clamp(62 + 4 * x, 64, 184)) <= 1, "MRT real crop X");
            require(abs(int(result[i + 1]) - std::clamp(186 - 4 * y, 64, 184)) <= 1, "MRT real flip Y");
        }
    // Switching back to a single attachment must not retain the old MRT target.
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glDrawBuffers(1, attachments);
    glUniform1i(glGetUniformLocation(p, "captureReal"), 0);
    draw();
    require(result == aligned, "single-target capture after MRT");
    glDeleteTextures(1, &real);
    glDeleteTextures(1, &tex);
    glDeleteTextures(1, &out);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(p);
    glDeleteVertexArrays(1, &vao);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surf);
    eglDestroyContext(display, ctx);
    eglTerminate(display);
    puts("VR LSFG capture, crop, orientation and rotation shader tests passed");
}
