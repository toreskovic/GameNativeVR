#pragma once

#include "xr_visibility_mask.h"
#include "xr_windows_transport.h"
#include "xr_sgsr.h"
#include "xr_lsfg.h"
#include "xr_vulkan_present.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>

namespace xrimmersive::windowsvr {

class WindowsProjectionPresenter {
public:
    void setVisibilityMasks(std::array<VisibilityMesh,2> masks) {visibilityMasks_=std::move(masks);++visibilityRevision_;}
    bool initialize(XrSession session, int64_t format, uint32_t width, uint32_t height, EGLDisplay display, int upscaler, float sgsrSharpness, float fovScale, int fovBorder, bool fxaa, int ffrDebug, XrVulkanContext* vulkan = nullptr);
    bool render(WindowsFrameTransport &transport, XrSpace space, XrCompositionLayerProjection *layer, XrTime displayTime, XrDuration displayPeriod, int64_t displayDeadline);
    void shutdown();
    void prepareFrameGeneration(WindowsFrameTransport &transport);
    void resetFrameGeneration() { frameGenerator_.reset(); }

private:
    std::array<VisibilityMesh,2> visibilityMasks_;
    uint64_t visibilityRevision_=0;
    XrVulkanContext* vulkan_ = nullptr;
    std::unique_ptr<XrVulkanPresenter> vulkanPresenter_;
    bool renderVulkan(WindowsFrameTransport&, XrSpace, XrCompositionLayerProjection*, XrTime, XrDuration, int64_t);
    VrFrameGenerator frameGenerator_;
    VrFrameGenerator::Output captureForGeneration(WindowsFrameTransport &transport, XrTime displayTime, XrDuration period);
    std::array<GLuint,2> renderTextures_{};
    XrTime lastDisplayTime_ = 0;
    XrDuration displayPeriod_ = 13888889;
    bool displayedSynthetic_ = false, previousSynthetic_ = false;
    uint64_t generatedPairs_ = 0;
    bool hasPresentedImage_ = false;
    // Only frame IDs and target times are read from this non-owning metadata copy.
    std::array<EyeFrame, 2> displayedFrames_{};
    uint64_t reusedImages_ = 0;
    void recordPresentation(const std::array<EyeFrame, 2> &frames, XrTime displayTime, bool reused, bool synthetic = false);
    XrTime timingLogStart_ = 0;
    uint64_t presentedPairs_ = 0, repeatedPairs_ = 0, mixedPairs_ = 0, timedEyes_ = 0;
    std::array<uint64_t, 2> previousFrameIds_{};
    double latenessSumMs_ = 0, latenessMaxMs_ = 0;
    bool ensureProgram();
    EGLImageKHR createImageFromHardwareBuffer(AHardwareBuffer *buffer);
    EGLImageKHR createImageFromDmabuf(const EyeFrame &frame);
    bool waitForAcquireFence(int fenceFd);
    int createReleaseFence();
    bool uploadLinearDmabufToTexture(uint32_t eye, int imageIndex, const EyeFrame &frame,
                                    GLuint &texture, uint64_t &cachedRegistration);
    bool importEyeBuffer(uint32_t eye, EyeFrame &frame, bool &fresh);
    void drawEye(uint32_t eye, const EyeFrame &source, uint32_t imageIndex);
    void discardFresh(WindowsFrameTransport &transport, const std::array<EyeFrame, 2> &frames,
                      const std::array<bool, 2> &fresh);

    XrSession session_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    std::vector<XrSwapchainImageOpenGLESKHR> images_;
    std::array<XrCompositionLayerProjectionView, 2> views_{};
    std::array<std::array<EGLImageKHR, WindowsFrameTransport::kMaxImages>, 2> eglImages_{};
    std::array<std::array<GLuint, WindowsFrameTransport::kMaxImages>, 2> textures_{};
    std::array<std::array<uint64_t, WindowsFrameTransport::kMaxImages>, 2> registrations_{};
    std::array<std::array<bool, WindowsFrameTransport::kMaxImages>, 2> cpuFallback_{};
    std::array<std::array<void *, WindowsFrameTransport::kMaxImages>, 2> cpuMappings_{};
    std::array<std::array<size_t, WindowsFrameTransport::kMaxImages>, 2> cpuMappingLengths_{};
    std::array<std::array<uint64_t, WindowsFrameTransport::kMaxImages>, 2> cpuMappingRegistrations_{};
    std::array<std::array<int, WindowsFrameTransport::kMaxImages>, 2> cpuTextureWidths_{};
    std::array<std::array<int, WindowsFrameTransport::kMaxImages>, 2> cpuTextureHeights_{};
    std::array<uint64_t, 2> renderedSerials_{0, 0};
    EGLSyncKHR acquireSync_ = EGL_NO_SYNC_KHR;
    SgsrUpscaler sgsr_;
    float fovScale_ = 1.0f;
    int fovBorder_ = 0;
    uint32_t sceneWidth_ = 0, sceneHeight_ = 0;
    GLuint sceneTexture_ = 0, sceneFramebuffer_ = 0, borderProgram_ = 0;
    GLuint framebuffer_ = 0;
    GLuint program_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint vertexArray_ = 0;
    GLint uvTransformLocation_ = -1;
};

}
