#pragma once
#include <GLES3/gl3.h>

namespace xrimmersive::windowsvr {
// Single-pass SGSR v1, owned and used by the projection render thread.
class SgsrUpscaler {
public:
    bool initialize(bool edgeDirection = false, float sharpness = 0.7f);
    bool ready() const { return program_ != 0; }
    // The presenter supplies the bound eye texture on unit 0, output FBO,
    // viewport and full-screen VAO (position at 0, top-down UV at 1).
    void draw(int textureWidth, int textureHeight, int x, int y,
              int width, int height, bool flipY);
    void shutdown();
private:
    GLuint program_ = 0;
    GLint viewport_ = -1, bounds_ = -1, transform_ = -1, fullTexture_ = -1;
};
}
