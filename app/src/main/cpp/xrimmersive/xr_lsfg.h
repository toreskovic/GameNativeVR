#pragma once
#include "xr_windows_transport.h"
#include "xr_vulkan_context.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xrimmersive::windowsvr {
// Presentation/GLES calls run on the XR thread; Vulkan capture has a serialized worker.
class VrFrameGenerator {
  public:
    struct Output {
        std::array<GLuint, 2> textures{};
        std::array<EyeFrame, 2> frames{};
        bool synthetic = false;
        std::array<VkImage, 2> images{};
        std::array<VkImageView, 2> views{};
        explicit operator bool() const { return textures[0] && textures[1]; }
    };
    // Diagnostic duplicates only; the original acquire fences remain owned by
    // the capture path (GLES or Vulkan). Lifetime ends on completion or failure.
    struct GuestReadiness {
        explicit GuestReadiness(const std::array<EyeFrame, 2> &frames);
        ~GuestReadiness();
        GuestReadiness(const GuestReadiness &) = delete;
        GuestReadiness &operator=(const GuestReadiness &) = delete;
        std::array<int, 2> fds{-1, -1}; // -1 no dependency, -2 duplication failed
        int64_t claimedAt = 0;
    };
    VrFrameGenerator();
    ~VrFrameGenerator();
    void setVulkanContext(XrVulkanContext* context);
    // Prototype retained for research, but unavailable to sessions or saved settings.
    // Passthrough ownership/fence helpers below still serve ordinary presentation.
    constexpr bool enabled() const { return false; }
    bool canIngest() const;
    bool canPresentReal() const;
    // Non-blocking retirement only; never advances synthetic/real presentation.
    void pollCompletion();
    // Nonblocking selection of a completed, due image before swapchain acquisition.
    Output advance(int64_t displayTime, int64_t period, int64_t displayDeadline);
    Output ingest(EGLDisplay display, const std::array<GLuint, 2> &textures,
                  const std::array<EyeFrame, 2> &frames, int64_t displayTime, int64_t period,
                  const std::shared_ptr<GuestReadiness> &guestReadiness);
    // Returns false before consuming anything when the GLES fallback is needed.
    // A successful deferred capture returns kDeferredGuestFence; releaseGuest
    // keeps that guest lease until the worker observes readiness and captures it.
    static constexpr int kDeferredGuestFence = -2;
    bool ingestVulkan(EGLDisplay display, const std::array<EyeFrame, 2> &frames, int64_t displayTime,
                      int64_t period, const std::shared_ptr<GuestReadiness> &readiness, int &releaseFence);
    bool claimFrames(WindowsFrameTransport &transport, std::array<EyeFrame, 2> &frames);
    bool claimPassthrough(WindowsFrameTransport &transport, std::array<EyeFrame, 2> &frames, bool readyOnly = false);
    void cancelCapture();
    void releaseGuest(WindowsFrameTransport &transport, const std::array<EyeFrame, 2> &frames, int fence);
    void updateWorker(WindowsFrameTransport &transport, int64_t nextDisplayTime, int64_t period);
    void stopWorker();
    void finishRead(const std::array<GLuint, 2> &textures, int fence);
    void cancelPresentation();
    void notePresented(int64_t targetTime);
    void reset();
    void shutdown();

  private:
    XrVulkanContext* sharedContext_ = nullptr;
    mutable std::recursive_mutex mutex_;
    std::thread worker_;
    int wakeFd_ = -1;
    void notifyWorker();
    bool stopping_ = false;
    WindowsFrameTransport *transport_ = nullptr;
    int64_t workerTime_ = 0, workerPeriod_ = 0;
    bool claimed_ = false, pendingGuest_ = false;
    std::array<EyeFrame, 2> pendingFrames_{};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xrimmersive::windowsvr
