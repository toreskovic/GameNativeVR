#include "xr_performance.h"
#include "xr_lsfg.h"
#include "../lsfg/lsfg_chain.hpp"
#include "../lsfg/lsfg_dll.h"
#include "../lsfg/lsfg_shaders.hpp"
#include "xr_frame_interpolation.h"
#include "xr_lsfg_capture.h"
#include "xr_lsfg_shaders.h"
#include "xr_lsfg_timing.h"
#include "xr_optical_flow.h"
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <android/log.h>
#include <chrono>
#include <cstring>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan_android.h>

#define FGLOG(...) __android_log_print(ANDROID_LOG_INFO, "VrLsfg", __VA_ARGS__)

#include "xr_vulkan_dispatch.h"

namespace xrimmersive::windowsvr {
namespace {
bool fdReady(int fd) {
  if (fd < 0)
    return true;
  pollfd p{fd, POLLIN, 0};
  return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}
int64_t monotonicNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// Sync-file signal timestamps use CLOCK_MONOTONIC. Read only signaled fences;
// no waits, and no object/driver names are logged.
int64_t fenceSignalTime(int fd) {
  if (fd < 0)
    return 0;
  sync_file_info info{};
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0 || info.status != 1 ||
      !info.num_fences || info.num_fences > 32)
    return 0;
  std::array<sync_fence_info, 32> fences{};
  const uint32_t capacity = info.num_fences;
  info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0 || info.status != 1 ||
      info.num_fences > capacity)
    return 0;
  int64_t latest = 0;
  for (uint32_t i = 0; i < info.num_fences; ++i) {
    if (fences[i].status != 1 || !fences[i].timestamp_ns ||
        fences[i].timestamp_ns > INT64_MAX)
      return 0;
    latest = std::max(latest, int64_t(fences[i].timestamp_ns));
  }
  return latest;
}
struct TimingSamples {
  uint64_t count = 0;
  double sum = 0, maximum = 0;
  void add(int64_t ns) {
    const double ms = ns / 1e6;
    ++count;
    sum += ms;
    maximum = std::max(maximum, ms);
  }
  double average() const { return count ? sum / count : 0; }
};
// Three markers per independent command buffer. Results are read only after
// its fence retires; neither calibration nor diagnostics submit GPU work.
struct GpuJobTiming {
  struct Distribution {
    std::array<int64_t, 512> values{};
    size_t count = 0;
    TimingSamples samples;
    void add(int64_t ns) {
      if (ns < 0)
        return;
      samples.add(ns);
      values[count++ % values.size()] = ns;
    }
    double p95() const {
      auto copy = values;
      const auto n = std::min(count, copy.size());
      if (!n)
        return 0;
      std::sort(copy.begin(), copy.begin() + n);
      return copy[(n * 95 - 1) / 100] / 1e6;
    }
  } submitToEntry, entryToReady, execution, endToSignal, signalToWorker,
      calibrationCost;
  std::array<Distribution, 3> dependencyLate;
  std::array<int, 3> dependencies{-1, -1, -1};
  std::array<bool, 3> supplied{};
  uint64_t missingDependencies = 0, badQueries = 0, uncalibrated = 0,
           calibrated = 0;
  uint64_t implicitDependencies = 0, badCalibration = 0, maxDeviation = 0;
  VkTimeDomainEXT hostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
  std::string calibrationCapability = "extension absent";
  RawClockBridge rawBridge;
  uint64_t calibrationApiErrors = 0, bridgeErrors = 0;
  VkQueryPool pool{};
  int64_t submitted = 0;
  void resetJob() {
    for (auto &fd : dependencies) {
      if (fd >= 0)
        close(fd);
      fd = -1;
    }
    supplied = {};
    submitted = 0;
  }
  ~GpuJobTiming() { resetJob(); }
  void beginJob(std::array<int, 3> fds) {
    resetJob();
    for (size_t i = 0; i < fds.size(); ++i) {
      supplied[i] = fds[i] >= 0;
      if (supplied[i])
        dependencies[i] = dup(fds[i]);
    }
    if (hostDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT)
      rawBridge = sampleRawClockBridge();
    submitted = monotonicNs();
  }
  void collect(VkDevice device, uint32_t bits, double period, int64_t signal,
               int64_t observed, PFN_vkGetCalibratedTimestampsEXT calibrate) {
    if (!submitted)
      return;
    for (size_t i = 0; i < dependencies.size(); ++i) {
      const auto t = fenceSignalTime(dependencies[i]);
      if (!supplied[i])
        ++implicitDependencies;
      else if (t > 0 && t <= observed)
        dependencyLate[i].add(std::max<int64_t>(0, t - submitted));
      else
        ++missingDependencies;
    }
    if (signal > 0 && signal <= observed)
      signalToWorker.add(observed - signal);
    uint64_t ticks[3]{};
    if (!pool || vkGetQueryPoolResults(device, pool, 0, 3, sizeof(ticks), ticks,
                                       sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      ++badQueries;
      resetJob();
      return;
    }
    entryToReady.add(gpuTimestampDuration(ticks[0], ticks[1], bits, period));
    execution.add(gpuTimestampDuration(ticks[1], ticks[2], bits, period));
    if (!calibrate) {
      ++uncalibrated;
      resetJob();
      return;
    }
    VkCalibratedTimestampInfoEXT infos[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT}};
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
    infos[1].timeDomain = hostDomain;
    uint64_t clocks[2]{}, deviation = 0;
    const auto before = monotonicNs();
    const bool raw = hostDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
    if (raw) rawBridge.include(sampleRawClockBridge());
    const auto status = calibrate(device, 2, infos, clocks, &deviation);
    if (raw) rawBridge.include(sampleRawClockBridge());
    calibrationCost.add(monotonicNs() - before);
    if (status != VK_SUCCESS) ++calibrationApiErrors;
    const auto host = raw ? rawBridge.convert(clocks[1])
                         : clocks[1] < INT64_MAX ? int64_t(clocks[1]) : -1;
    if (raw && host <= 0) ++bridgeErrors;
    // Saturate before adding bridge uncertainty; never wrap an error bound.
    if (raw) deviation = deviation > 1000000 || rawBridge.uncertainty() > 1000000
        ? UINT64_MAX : deviation + rawBridge.uncertainty();
    maxDeviation = std::max(maxDeviation, deviation);
    const auto entry = calibratedGpuTime(ticks[0], clocks[0], host, bits, period);
    const auto end = calibratedGpuTime(ticks[2], clocks[0], host, bits, period);
    // Report millisecond-scale uncertainty as unavailable. Small negative
    // differences within the reported uncertainty may be rounded to zero.
    if (status != VK_SUCCESS || deviation > 1000000 || entry <= 0 || end <= 0 ||
        entry < submitted - int64_t(deviation) ||
        end > observed + int64_t(deviation) ||
        (signal > 0 && end > signal + int64_t(deviation)))
      ++badCalibration;
    else {
      ++calibrated;
      submitToEntry.add(std::max<int64_t>(0, entry - submitted));
      if (signal > 0)
        endToSignal.add(std::max<int64_t>(0, signal - end));
    }
    resetJob();
  }
  void log(const char *name) {
    // One compact line per category; each tuple is n/avg/p95/max in ms.
    auto tuple = [](const Distribution &d) {
      char value[96];
      snprintf(value, sizeof(value), "%llu/%.3f/%.3f/%.3f",
               (unsigned long long)d.samples.count, d.samples.average(),
               d.p95(), d.samples.maximum);
      return std::string(value);
    };
    FGLOG("queue breakdown %s (n/avg/p95/max ms): submitToEntry=%s "
          "entryToReady=%s readyToEnd=%s endToSignal=%s signalToWorker=%s",
          name, tuple(submitToEntry).c_str(), tuple(entryToReady).c_str(),
          tuple(execution).c_str(), tuple(endToSignal).c_str(),
          tuple(signalToWorker).c_str());
    FGLOG("queue dependencies %s (n/avg/p95/max ms): dependency0Late=%s "
          "dependency1Late=%s dependency2Late=%s calibrationCall=%s",
          name, tuple(dependencyLate[0]).c_str(),
          tuple(dependencyLate[1]).c_str(), tuple(dependencyLate[2]).c_str(),
          tuple(calibrationCost).c_str());
    FGLOG("queue breakdown %s coverage: calibrated=%llu unavailable=%llu "
          "rejected=%llu queryMiss=%llu "
          "dependencyImplicit=%llu dependencyUnknown=%llu "
          "calibrationMaxDeviationUs=%.3f capability=%s apiErrors=%llu bridgeErrors=%llu",
          name, (unsigned long long)calibrated,
          (unsigned long long)uncalibrated, (unsigned long long)badCalibration,
          (unsigned long long)badQueries,
          (unsigned long long)implicitDependencies,
          (unsigned long long)missingDependencies, maxDeviation / 1e3,
          calibrationCapability.c_str(), (unsigned long long)calibrationApiErrors,
          (unsigned long long)bridgeErrors);
    submitToEntry = {};
    entryToReady = {};
    execution = {};
    endToSignal = {};
    signalToWorker = {};
    calibrationCost = {};
    dependencyLate = {};
    calibrated = uncalibrated = badCalibration = badQueries = 0;
    implicitDependencies = missingDependencies = maxDeviation = 0;
    calibrationApiErrors = bridgeErrors = 0;
  }
};
GLuint program() {
  GLuint shaders[2]{};
  const char *src[] = {kLsfgCaptureVertex, kLsfgCaptureFragment};
  for (int i = 0; i < 2; ++i) {
    shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(shaders[i], 1, &src[i], nullptr);
    glCompileShader(shaders[i]);
    GLint ok = 0;
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      for (auto s : shaders)
        if (s)
          glDeleteShader(s);
      return 0;
    }
  }
  GLuint p = glCreateProgram();
  for (auto s : shaders)
    glAttachShader(p, s);
  glLinkProgram(p);
  for (auto s : shaders)
    glDeleteShader(s);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(p);
    return 0;
  }
  return p;
}
EyeFrame metadata(const EyeFrame &f, int w, int h) {
  EyeFrame out{};
  out.width = out.sourceWidth = w;
  out.height = out.sourceHeight = h;
  out.projectionValid = f.projectionValid;
  std::copy_n(f.projectionOrientation, 4, out.projectionOrientation);
  std::copy_n(f.projectionPosition, 3, out.projectionPosition);
  std::copy_n(f.projectionFov, 4, out.projectionFov);
  out.frameId = f.frameId;
  out.targetDisplayTime = f.targetDisplayTime;
  out.receivedAt = f.receivedAt;
  out.arrivalInterval = f.arrivalInterval;
  return out;
}
// Check the cache's actual variant, not its filename: extraction may fall back
// to another variant when a DLL does not contain the preferred shaders.
bool usableCache(const std::string &path, bool half,
                 const VkPhysicalDevice16BitStorageFeatures &storage) {
  if (path.empty())
    return false;
  LsfgModuleSet modules{};
  if (lsfg_load_modules(path.c_str(), &modules) != LSFG_OK)
    return false;
  bool ok = half ? modules.variant == LSFG_VARIANT_FP16
                 : modules.variant != LSFG_VARIANT_FP16;
  for (uint32_t i = 0; ok && i < modules.count; ++i) {
    const auto &m = modules.modules[i];
    for (uint32_t offset = 5; offset < m.word_count;) {
      uint32_t count = m.words[offset] >> 16;
      if (!count || count > m.word_count - offset) {
        ok = false;
        break;
      }
      if ((m.words[offset] & 0xffff) == 17 && count == 2) { // OpCapability
        switch (m.words[offset + 1]) {
        case 9:
          ok &= half;
          break; // Float16
        case 4433:
          ok &= storage.storageBuffer16BitAccess;
          break;
        case 4434:
          ok &= storage.uniformAndStorageBuffer16BitAccess;
          break;
        case 4435:
          ok &= storage.storagePushConstant16;
          break;
        case 4436:
          ok &= storage.storageInputOutput16;
          break;
        default:
          break;
        }
      }
      offset += count;
    }
  }
  lsfg_release_modules(&modules);
  return ok;
}
VkImageMemoryBarrier barrier(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout layout, VkAccessFlags src,
                             VkAccessFlags dst,
                             uint32_t from = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t to = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = oldLayout;
  b.newLayout = layout;
  b.srcQueueFamilyIndex = from;
  b.dstQueueFamilyIndex = to;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}
} // namespace
struct VrFrameGenerator::Impl {
  struct Shared {
    AHardwareBuffer *buffer = nullptr;
    EGLImageKHR egl = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::unique_ptr<lsfg::LsfgImage> owned;
  };
  XrVulkanContext* sharedContext = nullptr;
  uint32_t nextImageToken = 1;
  std::string cache, cacheFp16;
  bool opticalFlow = false;
  bool useFp16 = false;
  float flowScale = .5f;
  std::atomic<bool> failed{false};
  bool history = false, inFlight = false, discardJob = false;
  int width = 0, height = 0, current = 0;
  EGLDisplay display = EGL_NO_DISPLAY;
  GLuint shader = 0, fbo = 0, vao = 0;
  GLint captureRealLoc = -1, sourceLoc = -1, cropLoc = -1, warpLoc = -1,
        rotationLoc = -1, sourceFovLoc = -1, targetFovLoc = -1;
  GLuint real[2][2]{};
  std::array<EyeFrame, 2> meta[2]{};
  std::array<EyeFrame, 2> syntheticMeta{}, anchor{};
  FlowHistory flowHistory;
  Shared input[2][2], output[2], presentedOutput[2];
  // One completed stereo result and one generation bank, never a FIFO.
  struct Pending : BufferedResultState {
    bool decision = false;
    int fence = -1;
    int64_t submitted = 0, completed = 0, displayTime = 0;
    std::array<EyeFrame, 2> synthetic{}, real{};
    PresentationTimeline::Pair due{};
  } pending;
  int presentedReadFd = -1;
  PresentationTimeline timeline;
  PresentationTimeline::Pair jobDue{};
  bool resultAvailable = false, resultSynthetic = false;
  int64_t jobArrival = 0;
  TimingSamples scheduledHold, deadlineMiss, arrivalToCapture, arrivalToResult;
  void clearPending() {
    if (pending.fence >= 0)
      close(pending.fence);
    pending = {};
  }
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool timestampPool = VK_NULL_HANDLE;
  GpuJobTiming generationTiming, captureTiming;
  PFN_vkGetCalibratedTimestampsEXT calibratedTimestamps = nullptr;
  bool calibrationKhr = false, calibrationEnabled = false;
  uint32_t timestampBits = 0;
  float timestampPeriod = 0;
  int inputDiagnosticFd = -1,
      completionFd = -2; // -2 = not exported, -1 = already signaled
  TimingSamples gpuElapsed, inputWait, signalToPoll, afterInput;
  std::shared_ptr<GuestReadiness> guestReadiness;
  TimingSamples guestWait, captureAfterGuest, guestAfterClaim;
  uint64_t guestMisses = 0, implicitReadyPairs = 0;
  uint64_t timestampMisses = 0;
  std::unique_ptr<VulkanEyeCapture> capture;
  Shared realShared[2][2], captureRealShared[2];
  int captureRealReadFd = -1;
  bool directActive = false;
  bool onWorker = false, presentationLease = false;
  int readFds[3]{-1, -1, -1}; // real slot 0/1, synthetic pair
  int glReuseFd = -1; // last completed Vulkan use, for a later GLES fallback
  VkSemaphore reuseReady[3]{};
  bool directStorage = false;
  bool loggedDirect = false, loggedFallback = false;
  uint64_t vulkanCaptureJobs = 0, glesCaptureJobs = 0, workerCaptureJobs = 0;
  VkCommandBuffer captureCmd{};
  VkSemaphore captureReady{}, guestReady[2]{};
  // One independently captured pair. It never aliases active history or flow.
  std::unique_ptr<VulkanEyeCapture> aheadCapture;
  lsfg::LsfgImage aheadImages[2], rawHistory[2][2];
  int aheadFd = -1;
  std::shared_ptr<GuestReadiness> aheadReadiness;
  VkCommandBuffer aheadCmd{};
  VkFence aheadFence{};
  VkSemaphore aheadReady{}, aheadGuest[2]{};
  std::array<EyeFrame, 2> aheadFrames{};
  bool aheadPending = false, capturedReady = false, discardCapture = false,
       usingAhead = false;
  bool deferredCapture = false, captureInFlight = false;
  int captureRetireFd = -1;
  VkSemaphore captureWaits[3]{};
  VkPipelineStageFlags captureStages[3]{};
  uint32_t captureWaitCount = 0;
  uint64_t aheadCount = 0;
  int captureReleaseFd = -1;
  VkSemaphore glReady = VK_NULL_HANDLE, computeReady = VK_NULL_HANDLE;
  PFN_vkGetSemaphoreFdKHR exportFd = nullptr;
  lsfg::Device lsfgDevice; // Resources retain a pointer to this wrapper.
  std::unique_ptr<lsfg::LsfgShaders> shaders;
  std::unique_ptr<lsfg::LsfgChain> chains[2];
  std::unique_ptr<OpticalFlowBackend> opticalChains[2];
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahbProperties = nullptr;
  PFN_vkImportSemaphoreFdKHR importFd = nullptr;

  uint64_t generated = 0, late = 0, stale = 0;
  int64_t lastPresentedTime = 0;
  int64_t logTime = 0, submittedAt = 0, submittedDisplayTime = 0,
          lastFencePoll = 0;
  TimingSamples preparation, queueSubmit, completion, displayAge, pollGap;
  uint64_t reusedHistory = 0, primedHistory = 0;
  void logTimings(int64_t now, int64_t period) {
    if (!logTime) {
      logTime = now;
      return;
    }
    if (now - logTime < 5000000000LL)
      return;
    // Completion is observed on the XR thread: includes queueing, GL fence
    // waits, GPU execution and polling delay. It is NOT a GPU timestamp.
    FGLOG(
        "timing: completed=%llu generatedTotal=%llu lateTotal=%llu "
        "staleTotal=%llu historyReuse=%llu "
        "historyPrime=%llu "
        "periodMs=%.2f prepareMs(avg/max)=%.2f/%.2f "
        "queueSubmitMs(avg/max)=%.2f/%.2f "
        "observedCompleteMs(avg/max)=%.2f/%.2f displayAgeMs(avg/max)=%.2f/%.2f "
        "pollGapMaxMs=%.2f pendingAgeMs=%.2f",
        (unsigned long long)completion.count, (unsigned long long)generated,
        (unsigned long long)late, (unsigned long long)stale,
        (unsigned long long)reusedHistory, (unsigned long long)primedHistory,
        period / 1e6, preparation.average(), preparation.maximum,
        queueSubmit.average(), queueSubmit.maximum, completion.average(),
        completion.maximum, displayAge.average(), displayAge.maximum,
        pollGap.maximum, inFlight ? (now - submittedAt) / 1e6 : 0.0);
    FGLOG("stages: gpuSamples=%llu gpuElapsedMs(avg/max)=%.2f/%.2f "
          "queryMisses=%llu "
          "inputSamples=%llu inputWaitMs(avg/max)=%.2f/%.2f "
          "outputSamples=%llu signalToPollMs(avg/max)=%.2f/%.2f "
          "afterInputSamples=%llu afterInputMs(avg/max)=%.2f/%.2f "
          "decisionSamples=%llu signalToDecisionMs(avg/max)=%.2f/%.2f",
          (unsigned long long)gpuElapsed.count, gpuElapsed.average(),
          gpuElapsed.maximum, (unsigned long long)timestampMisses,
          (unsigned long long)inputWait.count, inputWait.average(),
          inputWait.maximum, (unsigned long long)signalToPoll.count,
          signalToPoll.average(), signalToPoll.maximum,
          (unsigned long long)afterInput.count, afterInput.average(),
          afterInput.maximum, (unsigned long long)signalToDecision.count,
          signalToDecision.average(), signalToDecision.maximum);
    FGLOG("input breakdown: samples=%llu guestWaitMs(avg/max)=%.2f/%.2f "
          "captureAfterGuestMs(avg/max)=%.2f/%.2f "
          "guestReadyAfterClaimMs(avg/max)=%.2f/%.2f "
          "unavailable=%llu implicitReadyPairs=%llu vulkanCaptureJobs=%llu "
          "glesCaptureJobs=%llu "
          "workerCaptureJobs=%llu",
          (unsigned long long)guestWait.count, guestWait.average(),
          guestWait.maximum, captureAfterGuest.average(),
          captureAfterGuest.maximum, guestAfterClaim.average(),
          guestAfterClaim.maximum, (unsigned long long)guestMisses,
          (unsigned long long)implicitReadyPairs,
          (unsigned long long)vulkanCaptureJobs,
          (unsigned long long)glesCaptureJobs,
          (unsigned long long)workerCaptureJobs);
    FGLOG("timeline: cadenceMs=%.2f arrivalToCaptureMs(avg/max)=%.2f/%.2f "
          "arrivalToResultMs(avg/max)=%.2f/%.2f "
          "scheduledHoldMs(avg/max)=%.2f/%.2f "
          "deadlineOverrunMs(avg/max)=%.2f/%.2f",
          timeline.cadence() / 1e6, arrivalToCapture.average(),
          arrivalToCapture.maximum, arrivalToResult.average(),
          arrivalToResult.maximum, scheduledHold.average(),
          scheduledHold.maximum, deadlineMiss.average(), deadlineMiss.maximum);
    generationTiming.log("generation");
    captureTiming.log("capture");
    arrivalToCapture = {};
    arrivalToResult = {};
    scheduledHold = {};
    deadlineMiss = {};
    guestWait = {};
    captureAfterGuest = {};
    guestAfterClaim = {};
    guestMisses = implicitReadyPairs = 0;
    vulkanCaptureJobs = glesCaptureJobs = workerCaptureJobs = 0;
    gpuElapsed = {};
    inputWait = {};
    signalToPoll = {};
    afterInput = {};
    signalToDecision = {};
    timestampMisses = 0;
    preparation = {};
    queueSubmit = {};
    completion = {};
    displayAge = {};
    pollGap = {};
    reusedHistory = primedHistory = 0;
    logTime = now;
  }

  void fail(const char *why) {
    if (!failed)
      FGLOG("disabled; using real stereo frames: %s", why);
    failed = true;
    // Free failed allocations promptly; never free resources still referenced
    // by an outstanding GPU job (including a device-loss/timeout path).
    if (!inFlight && !onWorker)
      clearImages();
    else if (!inFlight && device)
      { if(sharedContext) sharedContext->idle(); else vkDeviceWaitIdle(device); }
  }
  bool initDevice() {
    auto getInstance = sharedContext ? sharedContext->get : vkGetInstanceProcAddr;
    if (sharedContext) {
      instance=sharedContext->instance;physical=sharedContext->physical;
      device=sharedContext->device;family=sharedContext->family;
      uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
      std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());
      timestampBits=queues[family].timestampValidBits;
      timestampPeriod=sharedContext->properties.limits.timestampPeriod;
      vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr);
      std::vector<VkExtensionProperties> exts(count);vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,exts.data());
      for(auto& e:exts) {
        if(!strcmp(e.extensionName,"VK_KHR_calibrated_timestamps")) calibrationKhr=calibrationEnabled=true;
        if(!strcmp(e.extensionName,VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) calibrationEnabled=true;
      }
    } else {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "GameNative VR interpolation";
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion)
      enumerateVersion(&instanceVersion);
    if (instanceVersion < VK_API_VERSION_1_1)
      return false;
    app.apiVersion = std::min(instanceVersion, uint32_t(VK_API_VERSION_1_2));
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
      return false;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (auto candidate : devices) {
      uint32_t n = 0;
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, nullptr);
      std::vector<VkExtensionProperties> exts(n);
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, exts.data());
      auto has = [&](const char *s) {
        return std::any_of(exts.begin(), exts.end(), [&](const auto &e) {
          return !strcmp(e.extensionName, s);
        });
      };
      if (!has(
              VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) ||
          !has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) ||
          !has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
        continue;
      uint32_t nq = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, nullptr);
      std::vector<VkQueueFamilyProperties> qs(nq);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, qs.data());
      uint32_t q = 0;
      for (; q < nq; ++q)
        if ((qs[q].queueFlags &
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
          break;
      if (q == nq)
        continue;
      VkPhysicalDeviceFeatures2 features{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      VkPhysicalDeviceVulkanMemoryModelFeatures model{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
      VkPhysicalDeviceShaderFloat16Int8Features half{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
      VkPhysicalDevice16BitStorageFeatures storage{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      const bool core12 = app.apiVersion >= VK_API_VERSION_1_2 &&
                          properties.apiVersion >= VK_API_VERSION_1_2;
      const bool halfExtension = has(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
      const bool halfAvailable = halfExtension || core12;
      features.pNext = &storage;
      if (halfAvailable) {
        half.pNext = features.pNext;
        features.pNext = &half;
      }
      const bool memoryModelExtension =
          has(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
      const bool memoryModel = memoryModelExtension || core12;
      if (memoryModel) {
        model.pNext = features.pNext;
        features.pNext = &model;
      }
      vkGetPhysicalDeviceFeatures2(candidate, &features);
      if (!features.features.shaderStorageImageWriteWithoutFormat ||
          !features.features.shaderStorageImageExtendedFormats)
        continue;
      useFp16 = !opticalFlow && halfAvailable && half.shaderFloat16 &&
                usableCache(cacheFp16, true, storage);
      // Do not accidentally run a fallback FP16 cache on a device lacking
      // support.
      if (!opticalFlow && !useFp16 && !usableCache(cache, false, storage))
        continue;
      half.shaderInt8 = VK_FALSE;
      half.shaderFloat16 = useFp16 ? VK_TRUE : VK_FALSE;
      VkPhysicalDeviceFeatures enabled{};
      enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
      enabled.shaderStorageImageExtendedFormats = VK_TRUE;
      std::vector<const char *> extensions = {
          VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
          VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
      const bool calKhr = has("VK_KHR_calibrated_timestamps");
      const bool calExt = has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
      if (calKhr || calExt)
        extensions.push_back(calKhr
                                 ? "VK_KHR_calibrated_timestamps"
                                 : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
      if (memoryModelExtension)
        extensions.push_back(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
      if (useFp16 && halfExtension)
        extensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
      float priority = 1;
      VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      qi.queueFamilyIndex = q;
      qi.queueCount = 1;
      qi.pQueuePriorities = &priority;
      VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      di.pQueueCreateInfos = &qi;
      di.queueCreateInfoCount = 1;
      di.pEnabledFeatures = &enabled;
      di.enabledExtensionCount = extensions.size();
      di.ppEnabledExtensionNames = extensions.data();
      // Only chain features enabled on this device. Storage16 is core in 1.1.
      di.pNext = &storage;
      if (useFp16) {
        half.pNext = const_cast<void *>(di.pNext);
        di.pNext = &half;
      }
      if (memoryModel) {
        model.pNext = const_cast<void *>(di.pNext);
        di.pNext = &model;
      }
      VkResult created = vkCreateDevice(candidate, &di, nullptr, &device);
      if (created != VK_SUCCESS && useFp16 &&
          usableCache(cache, false, storage)) {
        useFp16 = false;
        if (halfExtension)
          extensions.pop_back();
        di.enabledExtensionCount = extensions.size();
        di.pNext = &storage;
        if (memoryModel) {
          model.pNext = &storage;
          di.pNext = &model;
        }
        created = vkCreateDevice(candidate, &di, nullptr, &device);
      }
      if (created != VK_SUCCESS)
        continue;
      physical = candidate;
      calibrationEnabled = calKhr || calExt;
      calibrationKhr = calKhr;
      family = q;
      timestampBits = qs[q].timestampValidBits;
      timestampPeriod = properties.limits.timestampPeriod;
      break;
    }
    }
    if (!device || (!sharedContext && !vkd_load(instance, device, getInstance))) return false;
    auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(instance,"vkGetDeviceProcAddr"));
    vkGetDeviceQueue(device, family, 0, &queue);
    if (timestampBits && timestampPeriod > 0) {
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query.queryCount = 3;
      if (vkCreateQueryPool(device, &query, nullptr, &timestampPool) !=
          VK_SUCCESS)
        timestampPool = VK_NULL_HANDLE;
      generationTiming.pool = timestampPool;
      if (vkCreateQueryPool(device, &query, nullptr, &captureTiming.pool) !=
          VK_SUCCESS)
        captureTiming.pool = VK_NULL_HANDLE;
    }
    if (calibrationEnabled) {
      auto domains =
          reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
              getInstance(
                  instance,
                  calibrationKhr
                      ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"
                      : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
      auto get = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
          getDevice(device, calibrationKhr
                                          ? "vkGetCalibratedTimestampsKHR"
                                          : "vkGetCalibratedTimestampsEXT"));
      uint32_t n = 0;
      VkResult status = VK_SUCCESS;
      bool deviceDomain = false, mono = false, raw = false;
      std::string reason;
      if (!domains) reason = "missing-domains-function";
      else if (!get) reason = "missing-calibration-function";
      else if ((status = domains(physical, &n, nullptr)) != VK_SUCCESS)
        reason = "domain-count-error";
      else if (!n || n >= 64) reason = "invalid-domain-count";
      else {
        std::vector<VkTimeDomainEXT> supported(n);
        status = domains(physical, &n, supported.data());
        if (status != VK_SUCCESS) reason = "domain-list-error";
        else {
          supported.resize(n);
          for (auto domain : supported) {
            deviceDomain |= domain == VK_TIME_DOMAIN_DEVICE_EXT;
            mono |= domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
            raw |= domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
          }
          if (!deviceDomain) reason = "missing-device-domain";
          else if (!mono && !raw) reason = "missing-host-domain";
          else {
            calibratedTimestamps = get;
            generationTiming.hostDomain = captureTiming.hostDomain = mono
                ? VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT
                : VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
            reason = mono ? "MONOTONIC" : "MONOTONIC_RAW-bridged";
          }
        }
      }
      char capability[256];
      snprintf(capability, sizeof(capability),
               "%s:%s(status=%d,count=%u,device=%d,mono=%d,raw=%d)",
               calibrationKhr ? "KHR" : "EXT", reason.c_str(), int(status), n,
               int(deviceDomain), int(mono), int(raw));
      generationTiming.calibrationCapability = capability;
    }
    captureTiming.calibrationCapability = generationTiming.calibrationCapability;
    FGLOG("CPU/GPU calibration: %s; dependency "
          "capture[guestL,guestR,recycledReal] "
          "generation[recycledSynthetic,input,unused]",
          generationTiming.calibrationCapability.c_str());
    FGLOG("GPU timestamp diagnostics: %s (bits=%u periodNs=%.3f)",
          timestampPool ? "available" : "unavailable", timestampBits,
          timestampPeriod);
    ahbProperties =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)getDevice(
            device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    importFd = (PFN_vkImportSemaphoreFdKHR)getDevice(
        device, "vkImportSemaphoreFdKHR");
    exportFd = (PFN_vkGetSemaphoreFdKHR)getDevice(
        device, "vkGetSemaphoreFdKHR");
    if (!ahbProperties || !importFd || !exportFd)
      return false;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device, &pi, nullptr, &pool) != VK_SUCCESS)
      return false;
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS)
      return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS)
      return false;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &si, nullptr, &glReady) != VK_SUCCESS)
      return false;
    VkExportSemaphoreCreateInfo exportInfo{
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    si.pNext = &exportInfo;
    if (vkCreateSemaphore(device, &si, nullptr, &computeReady) != VK_SUCCESS)
      return false;
    capture = std::make_unique<VulkanEyeCapture>(device, ahbProperties);
    directStorage = capture->valid();
    if (directStorage) {
      if (!opticalFlow &&
          (vkAllocateCommandBuffers(device, &ai, &captureCmd) != VK_SUCCESS ||
           vkCreateSemaphore(device, &si, nullptr, &captureReady) !=
               VK_SUCCESS))
        directStorage = false;
      si.pNext = nullptr;
      for (auto &sem : reuseReady)
        if (vkCreateSemaphore(device, &si, nullptr, &sem) != VK_SUCCESS)
          directStorage = false;
      for (auto &sem : guestReady)
        if (vkCreateSemaphore(device, &si, nullptr, &sem) != VK_SUCCESS)
          directStorage = false;
    }
    lsfgDevice = lsfg::Device(device, physical);
    if (directStorage) {
      aheadCapture = std::make_unique<VulkanEyeCapture>(device, ahbProperties);
      if (!aheadCapture->valid() ||
          vkAllocateCommandBuffers(device, &ai, &aheadCmd) != VK_SUCCESS ||
          vkCreateFence(device, &fi, nullptr, &aheadFence) != VK_SUCCESS)
        return false;
      si.pNext = &exportInfo;
      if (vkCreateSemaphore(device, &si, nullptr, &aheadReady) != VK_SUCCESS)
        return false;
      si.pNext = nullptr;
      for (auto &sem : aheadGuest)
        if (vkCreateSemaphore(device, &si, nullptr, &sem) != VK_SUCCESS)
          return false;
    }
    if (!opticalFlow) {
      shaders = std::make_unique<lsfg::LsfgShaders>(
          lsfgDevice, useFp16 ? cacheFp16 : cache);
      if (!shaders->IsValid() && useFp16) {
        useFp16 = false;
        VkPhysicalDevice16BitStorageFeatures storage{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        if (!usableCache(cache, false, storage))
          return false;
        shaders = std::make_unique<lsfg::LsfgShaders>(lsfgDevice, cache);
      }
      if (!shaders->IsValid())
        return false;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    FGLOG("native stereo interpolation ready: %s (%s, %s, 2x, flow %.2f)",
          props.deviceName, useFp16 ? "FP16" : "FP32/integer",
          opticalFlow ? "FidelityFX optical flow + custom synthesis"
                      : "LSFG performance model",
          flowScale);
    return true;
  }
  bool shared(Shared &s, bool storage, int w = 0, int h = 0,
              bool flow = false) {
    if (sharedContext) {
      s.owned=std::make_unique<lsfg::LsfgImage>(lsfgDevice,
          VkExtent2D{uint32_t(w?w:width),uint32_t(h?h:height)},VK_FORMAT_R8G8B8A8_UNORM);
      if(!s.owned->Valid()) return false;
      s.image=s.owned->Handle();s.view=s.owned->View();
      s.texture=nextImageToken++; // Opaque presentation token, never a GLES object.
      return true;
    }
    if (!w)
      w = width;
    if (!h)
      h = height;
    AHardwareBuffer_Desc desc{};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = flow ? AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT
                       : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (AHardwareBuffer_allocate(&desc, &s.buffer))
      return false;
    VkAndroidHardwareBufferFormatPropertiesANDROID fmt{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID p{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    p.pNext = &fmt;
    if (ahbProperties(device, s.buffer, &p) != VK_SUCCESS ||
        fmt.format !=
            (flow ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM) ||
        (flow && !(fmt.formatFeatures &
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ||
        (storage &&
         !(fmt.formatFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)))
      return false;
    VkExternalMemoryImageCreateInfo ext{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.pNext = &ext;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = fmt.format;
    image.extent = {(uint32_t)w, (uint32_t)h, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (storage)
      image.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (vkCreateImage(device, &image, nullptr, &s.image) != VK_SUCCESS)
      return false;
    VkImportAndroidHardwareBufferInfoANDROID imp{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.buffer = s.buffer;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s.image;
    dedicated.pNext = &imp;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &dedicated;
    alloc.allocationSize = p.allocationSize;
    if (!p.memoryTypeBits)
      return false;
    alloc.memoryTypeIndex = __builtin_ctz(p.memoryTypeBits);
    if (vkAllocateMemory(device, &alloc, nullptr, &s.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS)
      return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &s.view) != VK_SUCCESS)
      return false;
    auto native = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
        "eglGetNativeClientBufferANDROID");
    auto create =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    auto bind = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    if (!native || !create || !bind)
      return false;
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    s.egl = create(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                   native(s.buffer), attrs);
    if (s.egl == EGL_NO_IMAGE_KHR)
      return false;
    glGenTextures(1, &s.texture);
    glBindTexture(GL_TEXTURE_2D, s.texture);
    bind(GL_TEXTURE_2D, s.egl);
    textureParams();
    return glGetError() == GL_NO_ERROR;
  }
  static void textureParams() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  void destroyShared(Shared &s) {
    if(s.owned) { s={};return; }
    if (s.texture)
      glDeleteTextures(1, &s.texture);
    if (s.egl != EGL_NO_IMAGE_KHR) {
      auto destroy =
          (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
      if (destroy)
        destroy(display, s.egl);
    }
    if (s.view)
      vkDestroyImageView(device, s.view, nullptr);
    if (s.image)
      vkDestroyImage(device, s.image, nullptr);
    if (s.memory)
      vkFreeMemory(device, s.memory, nullptr);
    if (s.buffer)
      AHardwareBuffer_release(s.buffer);
    s = {};
  }
  void clearImages() {
    // Only used during teardown/resize, never for ordinary frame scheduling.
    if (device) { if(sharedContext) sharedContext->idle(); else vkDeviceWaitIdle(device); }
    if (display != EGL_NO_DISPLAY)
      glFinish();
    if (inputDiagnosticFd >= 0)
      close(inputDiagnosticFd);
    if (completionFd >= 0)
      close(completionFd);
    inputDiagnosticFd = -1;
    completionFd = -2;
    guestReadiness.reset();
    generationTiming.resetJob();
    captureTiming.resetJob();
    for (auto &fd : readFds) {
      if (fd >= 0)
        close(fd);
      fd = -1;
    }
    presentationLease = false;
    clearPending();
    if (presentedReadFd >= 0)
      close(presentedReadFd);
    presentedReadFd = -1;
    if (glReuseFd >= 0)
      close(glReuseFd);
    glReuseFd = -1;
    if (captureReleaseFd >= 0)
      close(captureReleaseFd);
    captureReleaseFd = -1;
    for (auto &c : chains)
      c.reset();
    for (auto &c : opticalChains)
      c.reset();
    for (auto &pair : input)
      for (auto &s : pair)
        destroyShared(s);
    for (auto &s : output)
      destroyShared(s);
    for (auto &s : presentedOutput)
      destroyShared(s);
    for (int slot = 0; slot < 2; ++slot)
      for (int e = 0; e < 2; ++e) {
        if (realShared[slot][e].buffer || realShared[slot][e].owned)
          destroyShared(realShared[slot][e]);
        else if (real[slot][e])
          glDeleteTextures(1, &real[slot][e]);
        real[slot][e] = 0;
      }
    for (auto &image : captureRealShared)
      destroyShared(image);
    if (captureRealReadFd >= 0)
      close(captureRealReadFd);
    captureRealReadFd = -1;
    for (auto &image : aheadImages)
      image = {};
    for (auto &pair : rawHistory)
      for (auto &image : pair)
        image = {};
    if (aheadFd >= 0)
      close(aheadFd);
    aheadFd = -1;
    aheadReadiness.reset();
    if (captureRetireFd >= 0) close(captureRetireFd);
    captureRetireFd = -1;
    deferredCapture = captureInFlight = false;
    timeline.reset();
    aheadPending = capturedReady = discardCapture = usingAhead = false;
    history = false;
    flowHistory.reset();
    inFlight = false;
    discardJob = false;
    resultAvailable = false;
    width = height = 0;
  }
  bool compatiblePair(const std::array<EyeFrame, 2> &frames,
                      int64_t period) const {
    if (!history || !interpolationInterval(meta[current][0].targetDisplayTime,
                                           frames[0].targetDisplayTime, period))
      return false;
    for (int e = 0; e < 2; ++e) {
      const auto &f = frames[e];
      const auto &old = meta[current][e];
      if ((f.sourceWidth > 0 ? f.sourceWidth : f.width) != width ||
          (f.sourceHeight > 0 ? f.sourceHeight : f.height) != height ||
          !f.projectionValid || !old.projectionValid)
        return false;
      float dot = 0, dist = 0;
      for (int i = 0; i < 4; ++i)
        dot += f.projectionOrientation[i] * old.projectionOrientation[i];
      for (int i = 0; i < 3; ++i) {
        float d = f.projectionPosition[i] - old.projectionPosition[i];
        dist += d * d;
      }
      if (!(std::abs(dot) > .97f && dist < .04f))
        return false;
      for (int i = 0; i < 4; ++i)
        if (!(std::abs(f.projectionFov[i] - old.projectionFov[i]) < .02f))
          return false;
    }
    return true;
  }
  bool retireCapture() {
    if (!captureInFlight) return true;
    if (!fdReady(captureRetireFd)) return false;
    const auto signal = fenceSignalTime(captureRetireFd);
    const auto now = monotonicNs();
    captureTiming.collect(device, timestampBits, timestampPeriod, signal, now,
                          calibratedTimestamps);
    const auto arrival = std::max(aheadFrames[0].receivedAt, aheadFrames[1].receivedAt);
    if (arrival > 0) arrivalToCapture.add(std::max<int64_t>(0, (signal > 0 ? signal : now) - arrival));
    if (captureRetireFd >= 0) close(captureRetireFd);
    captureRetireFd = -1;
    captureInFlight = false;
    if (aheadPending) capturedReady = true;
    return true;
  }
  VkSubmitInfo captureBatch() {
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.waitSemaphoreCount = captureWaitCount;
    s.pWaitSemaphores = captureWaits;
    s.pWaitDstStageMask = captureStages;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &aheadCmd;
    s.signalSemaphoreCount = 1;
    s.pSignalSemaphores = &aheadReady;
    return s;
  }
  bool exportCapture(int &releaseFd) {
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = aheadReady;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    if (exportFd(device, &info, &releaseFd) != VK_SUCCESS) {
      if (sharedContext) sharedContext->idle(); else vkQueueWaitIdle(queue);
      fail("capture fence export failed");
      return false;
    }
    if (aheadFd >= 0) close(aheadFd);
    aheadFd = releaseFd >= 0 ? dup(releaseFd) : -1;
    captureRetireFd = releaseFd >= 0 ? dup(releaseFd) : -1;
    if (releaseFd >= 0 && (aheadFd < 0 || captureRetireFd < 0)) {
      if (sharedContext) sharedContext->idle(); else vkQueueWaitIdle(queue);
      fail("capture fence duplication failed");
      return false;
    }
    captureInFlight = true;
    return true;
  }
  bool captureAhead(const std::array<EyeFrame, 2> &frames, int &releaseFd,
                    bool defer = false) {
    releaseFd = -1;
    if (!retireCapture() || !aheadCapture || !aheadCapture->import(frames))
      return false;
    if (vkResetCommandBuffer(aheadCmd, 0) != VK_SUCCESS)
      return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(aheadCmd, &begin) != VK_SUCCESS)
      return false;
    if (captureTiming.pool) {
      vkCmdResetQueryPool(aheadCmd, captureTiming.pool, 0, 3);
      vkCmdWriteTimestamp(aheadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          captureTiming.pool, 0);
    }
    VkSemaphore waits[3]{};
    VkPipelineStageFlags stages[3] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    uint32_t count = 0;
    auto dependency = [&](int fd, VkSemaphore sem) {
      if (fd < 0)
        return true;
      int copy = dup(fd);
      if (copy < 0)
        return false;
      VkImportSemaphoreFdInfoKHR imp{
          VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      imp.semaphore = sem;
      imp.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
      imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      imp.fd = copy;
      if (importFd(device, &imp) != VK_SUCCESS) {
        close(copy);
        return false;
      }
      waits[count++] = sem;
      return true;
    };
    if (!dependency(captureRealReadFd, reuseReady[0]))
      return false;
    for (int e = 0; e < 2; ++e) {
      if (!dependency(frames[e].acquireFenceFd, aheadGuest[e]))
        return false;
      auto &realImage = captureRealShared[e];
      VkImageMemoryBarrier pre[] = {
          barrier(aheadCapture->sourceImage(e), VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
                  VK_QUEUE_FAMILY_FOREIGN_EXT, family),
          barrier(sharedContext ? realImage.image : aheadImages[e].Handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT),
          barrier(realImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : family)};
      pre[0].subresourceRange.baseArrayLayer = frames[e].bufferLayer;
      vkCmdPipelineBarrier(aheadCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, sharedContext ? 2 : 3, pre);
      if (e == 0 && captureTiming.pool)
        vkCmdWriteTimestamp(aheadCmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            captureTiming.pool, 1);
      aheadCapture->record(aheadCmd, e, aheadCapture->sourceView(e),
                           sharedContext ? realImage.view : aheadImages[e].View(), realImage.view, frames[e],
                           frames[e], width, height, !sharedContext, false);
      VkImageMemoryBarrier post[] = {
          barrier(aheadCapture->sourceImage(e), VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, 0, family,
                  VK_QUEUE_FAMILY_FOREIGN_EXT),
          barrier(realImage.image, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, 0,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : family,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT)};
      post[0].subresourceRange.baseArrayLayer = frames[e].bufferLayer;
      vkCmdPipelineBarrier(aheadCmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, 2, post);
    }
    if (captureTiming.pool)
      vkCmdWriteTimestamp(aheadCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          captureTiming.pool, 2);
    if (vkEndCommandBuffer(aheadCmd) != VK_SUCCESS ||
        vkResetFences(device, 1, &aheadFence) != VK_SUCCESS)
      return false;
    captureWaitCount = count;
    std::copy_n(waits, count, captureWaits);
    std::copy_n(stages, count, captureStages);
    captureTiming.beginJob({frames[0].acquireFenceFd, frames[1].acquireFenceFd,
                            captureRealReadFd});
    deferredCapture = defer;
    if (!defer) {
      const auto batch = captureBatch();
      if ((sharedContext ? sharedContext->submit(1, &batch, aheadFence)
                         : vkQueueSubmit(queue, 1, &batch, aheadFence)) != VK_SUCCESS)
        return false;
      if (!exportCapture(releaseFd)) return false;
    }
    for (int e = 0; e < 2; ++e)
      aheadFrames[e] = metadata(frames[e], width, height);
    aheadReadiness = std::make_shared<GuestReadiness>(frames);
    aheadPending = true;
    capturedReady = false;
    if (++aheadCount == 1)
      FGLOG("unified capture: immutable stereo snapshots; "
            "event-driven timeline");
    return true;
  }
  bool resize(int w, int h) {
    if (w == width && h == height)
      return true;
    clearImages();
    width = w;
    height = h;
    if (!sharedContext && !shader) {
      shader = program();
      if (shader) {
        sourceLoc = glGetUniformLocation(shader, "source");
        captureRealLoc = glGetUniformLocation(shader, "captureReal");
        cropLoc = glGetUniformLocation(shader, "crop");
        warpLoc = glGetUniformLocation(shader, "warp");
        rotationLoc = glGetUniformLocation(shader, "rotation");
        sourceFovLoc = glGetUniformLocation(shader, "sourceFov");
        targetFovLoc = glGetUniformLocation(shader, "targetFov");
      }
    }
    if ((sharedContext && !directStorage) || (!sharedContext && !shader))
      return false;
    if (!sharedContext && !fbo)
      glGenFramebuffers(1, &fbo);
    if (!sharedContext && !vao)
      glGenVertexArrays(1, &vao);
    for (int slot = 0; slot < 2; ++slot)
      for (int e = 0; e < 2; ++e) {
        if (directStorage) {
          if (!shared(realShared[slot][e], true)) {
            if (sharedContext) return false;
            directStorage = false;
            clearImages();
            FGLOG("Vulkan capture storage unavailable; using GLES capture");
            return resize(w, h);
          }
          real[slot][e] = realShared[slot][e].texture;
        } else {
          glGenTextures(1, &real[slot][e]);
          glBindTexture(GL_TEXTURE_2D, real[slot][e]);
          textureParams();
          glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        }
      }
    if (directStorage)
      for (auto &image : captureRealShared)
        if (!shared(image, true)) {
          if (sharedContext) return false;
          directStorage = false;
          clearImages();
          FGLOG("Independent capture storage unavailable; using GLES capture");
          return resize(w, h);
        }
    if (directStorage && !sharedContext)
      for (auto &pair : rawHistory)
        for (auto &image : pair) {
          image = lsfg::LsfgImage(lsfgDevice, {uint32_t(w), uint32_t(h)},
                                  VK_FORMAT_R8G8B8A8_UNORM);
          if (!image.Valid())
            return false;
        }
    if (directStorage && !sharedContext)
      for (auto &image : aheadImages) {
        image = lsfg::LsfgImage(lsfgDevice, {uint32_t(w), uint32_t(h)},
                                VK_FORMAT_R8G8B8A8_UNORM);
        if (!image.Valid())
          return false;
      }
    for (int e = 0; e < 2; ++e) {
      for (int i = 0; i < 2; ++i)
        if (!(directStorage && opticalFlow) &&
            !shared(input[e][i], directStorage)) {
          if (!directStorage)
            return false;
          if (sharedContext) return false;
          directStorage = false;
          clearImages();
          FGLOG("Vulkan capture input storage unavailable; using GLES capture");
          return resize(w, h);
        }
      // Keep synthesized color independent of aligned history: GLES reads
      // this output while capture can prepare the next history pair.
      if (!shared(output[e], true) || !shared(presentedOutput[e], true))
        return false;
      if (opticalFlow) {
        opticalChains[e] = std::make_unique<OpticalFlowBackend>(
            lsfgDevice, VkExtent2D{uint32_t(w), uint32_t(h)}, flowScale);
        if (!opticalChains[e]->valid())
          return false;
      } else {
        lsfg::LsfgImagePair externalFrames;
        for (int i = 0; i < 2; ++i)
          externalFrames[i] = lsfg::LsfgImage(
              input[e][i].image, input[e][i].view, {(uint32_t)w, (uint32_t)h},
              VK_FORMAT_R8G8B8A8_UNORM);
        chains[e] = std::make_unique<lsfg::LsfgChain>(
            lsfgDevice, *shaders, VkExtent2D{(uint32_t)w, (uint32_t)h},
            VK_FORMAT_R8G8B8A8_UNORM, flowScale, &externalFrames);
        if (!chains[e]->Valid())
          return false;
        chains[e]->SetTarget(lsfgDevice, 1, 0, 0, output[e].view);
      }
    }
    FGLOG("stereo history allocated at %dx%d per eye; flowScale=%.3f "
          "synthesis=Vulkan image; private raw "
          "history + dynamic timeline",
          w, h, flowScale);
    return glGetError() == GL_NO_ERROR;
  }
  bool draw(GLuint src, GLuint dst, const EyeFrame &frame,
            const EyeFrame *target, GLuint realDst = 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           dst, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           realDst, 0);
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(realDst ? 2 : 1, attachments);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return false;
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(shader);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(sourceLoc, 0);
    glUniform1i(captureRealLoc, realDst ? 1 : 0);
    float w = frame.sourceWidth > 0 ? frame.sourceWidth : frame.width;
    float h = frame.sourceHeight > 0 ? frame.sourceHeight : frame.height;
    glUniform4f(cropLoc, float(frame.sourceX) / frame.width,
                (frame.sourceY + (frame.flipY ? h : 0)) / frame.height,
                w / frame.width, (frame.flipY ? -h : h) / frame.height);
    glUniform1i(warpLoc, target ? 1 : 0);
    if (target) {
      auto rotation = targetToSourceRotation(frame.projectionOrientation,
                                             target->projectionOrientation);
      glUniformMatrix3fv(rotationLoc, 1, GL_FALSE, rotation.data());
      float a[4], b[4];
      for (int i = 0; i < 4; ++i) {
        a[i] = std::tan(frame.projectionFov[i]);
        b[i] = std::tan(target->projectionFov[i]);
      }
      glUniform4fv(sourceFovLoc, 1, a);
      glUniform4fv(targetFovLoc, 1, b);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
  }
  void barriers(const std::vector<VkImageMemoryBarrier> &b,
                VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, b.size(),
                         b.data());
  }
  bool submit(int64_t time, const std::array<GLuint, 2> &textures,
              const std::array<EyeFrame, 2> &frames) {
    const int64_t prepareStart = monotonicNs();
    const int old = 1 - current;
    const bool fused = directActive && opticalFlow;
    bool reuse = flowHistory.matches(meta[old][0].targetDisplayTime);
    for (int e = 0; e < 2; ++e) {
      reuse &= compatibleFlowAnchor(meta[current][e].projectionOrientation,
                                    meta[current][e].projectionFov,
                                    anchor[e].projectionOrientation,
                                    anchor[e].projectionFov);
    }

    const uint64_t frameCount =
        flowHistory.begin(reuse, old, meta[current][0].targetDisplayTime);
    for (int e = 0; e < 2; ++e) {
      syntheticMeta[e] = meta[current][e];
      auto &m = syntheticMeta[e];
      auto q = midpointRotation(meta[old][e].projectionOrientation,
                                m.projectionOrientation);
      std::copy(q.begin(), q.end(), m.projectionOrientation);
      for (int i = 0; i < 3; ++i)
        m.projectionPosition[i] =
            (meta[old][e].projectionPosition[i] + m.projectionPosition[i]) *
            .5f;
      for (int i = 0; i < 4; ++i)
        m.projectionFov[i] =
            (meta[old][e].projectionFov[i] + m.projectionFov[i]) * .5f;
      m.targetDisplayTime =
          meta[old][e].targetDisplayTime +
          (m.targetDisplayTime - meta[old][e].targetDisplayTime) / 2;
      if (!reuse)
        anchor[e] = m;
      // The generated image lives in the anchor camera, while translation
      // and timestamp still refer to the pair midpoint. OpenXR timewarp
      // must receive the image's actual orientation/FOV, not the latest pose.
      std::copy_n(anchor[e].projectionOrientation, 4, m.projectionOrientation);
      std::copy_n(anchor[e].projectionFov, 4, m.projectionFov);
      if (!directActive && !reuse &&
          !draw(real[old][e], input[e][old].texture, meta[old][e], &anchor[e]))
        return false;
      // Capture aligned input and unwarped real history in one render pass.
      // Keep real pixels in their original camera for fallback/timewarp.
      if (!directActive && !draw(textures[e], input[e][current].texture,
                                 frames[e], &anchor[e], real[current][e]))
        return false;
    }
    if (!directActive) {
      auto create =
          (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
      auto dup = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress(
          "eglDupNativeFenceFDANDROID");
      auto destroy =
          (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
      if (!create || !dup || !destroy)
        return false;
      const EGLint attrs[] = {EGL_NONE};
      EGLSyncKHR sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
      if (sync == EGL_NO_SYNC_KHR)
        return false;
      glFlush();
      int fd = dup(display, sync);
      destroy(display, sync);
      if (fd < 0)
        return false;
      VkImportSemaphoreFdInfoKHR imp{
          VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      imp.semaphore = glReady;
      imp.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
      imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      imp.fd = fd;
      if (!directActive)
        inputDiagnosticFd =
            ::dup(fd); // Vulkan consumes the original on successful import.
      if (importFd(device, &imp) != VK_SUCCESS) {
        close(fd);
        return false;
      }
    }
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS ||
        (directActive && !fused &&
         vkResetCommandBuffer(captureCmd, 0) != VK_SUCCESS))
      return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    std::array<VkSemaphore, 5> waits{};
    uint32_t waitCount = 0;
    bool outputDependency = false;
    if (!directActive)
      waits[waitCount++] = glReady;
    if (directActive) {
      // Generation never reads real presentation storage. Only the
      // synthetic destination's previous GLES use can gate this batch.
      if (readFds[2] >= 0) {
        int fd = dup(readFds[2]);
        if (fd < 0)
          return false;
        VkImportSemaphoreFdInfoKHR dependency{
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        dependency.semaphore = reuseReady[2];
        dependency.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        dependency.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        dependency.fd = fd;
        if (importFd(device, &dependency) != VK_SUCCESS) {
          close(fd);
          return false;
        }
        outputDependency = true;
      }
      if (!fused) {
        // Align private snapshots before interpolation on the same queue.
        // The separate fence retains the existing input-stage diagnostic.
        std::swap(cmd, captureCmd);
        bool recorded = vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
        if (recorded) {
          for (int e = 0; e < 2; ++e) {

            for (int slot = 0; slot < 2; ++slot)
              barriers(
                  {barrier(input[e][slot].image,
                           slot == old && reuse ? VK_IMAGE_LAYOUT_GENERAL
                                                : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, 0,
                           VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_SHADER_WRITE_BIT,
                           sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : family)},
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            // Raw history stays on this Vulkan queue for its lifetime.
            // Reanchoring cannot acquire or delay a GLES presentation image.
            for (int slot : {old, current}) {
              if (slot == old && reuse)
                continue;
              auto &raw = rawHistory[slot][e];
              barriers(
                  {barrier(raw.Handle(), VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT)},
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
              capture->record(cmd, e * 2 + (slot == current), raw.View(),
                              input[e][slot].view, input[e][slot].view,
                              meta[slot][e], anchor[e], width, height, false);
            }
          }
          recorded = vkEndCommandBuffer(cmd) == VK_SUCCESS;
        }
        std::swap(cmd, captureCmd);
        if (!recorded)
          return false;
      }
    }
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
      return false;
    if (timestampPool) {
      vkCmdResetQueryPool(cmd, timestampPool, 0, 3);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool,
                          0);
    }
    if (directActive) {
      VkMemoryBarrier captureDone{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      captureDone.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      captureDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &captureDone, 0, nullptr, 0, nullptr);
    }
    if (timestampPool) {
      // The GL semaphore or same-queue capture barrier gates this timestamp.
      // Capture/input readiness remains outside the LSFG command interval.
      vkCmdWriteTimestamp(cmd,
                          directActive ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          timestampPool, 1);
    }
    for (int e = 0; e < 2; ++e) {
      if (!fused)
        for (auto &src : input[e]) {
          barriers({barrier(src.image, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_GENERAL,
                            directActive ? VK_ACCESS_SHADER_WRITE_BIT : 0,
                            VK_ACCESS_SHADER_READ_BIT,
                            directActive ? VK_QUEUE_FAMILY_IGNORED
                                         : VK_QUEUE_FAMILY_FOREIGN_EXT,
                            directActive ? VK_QUEUE_FAMILY_IGNORED : family)},
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

      if (!opticalFlow) {
        if (!reuse) {
          chains[e]->ResetHistory();
          chains[e]->DispatchShared(cmd, frameCount - 1);
        }
        chains[e]->DispatchShared(cmd, frameCount);
      }
      barriers({barrier(output[e].image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                        sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : family)},
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      if (fused) {
        const std::array<ColorTransform, 2> transforms{
            colorTransform(
                meta[old][e].projectionOrientation, meta[old][e].projectionFov,
                anchor[e].projectionOrientation, anchor[e].projectionFov),
            colorTransform(meta[current][e].projectionOrientation,
                           meta[current][e].projectionFov,
                           anchor[e].projectionOrientation,
                           anchor[e].projectionFov)};
        opticalChains[e]->record(cmd, sharedContext ? realShared[old][e].view : rawHistory[old][e].View(),
                                 sharedContext ? realShared[current][e].view : rawHistory[current][e].View(), output[e].view,
                                 current, reuse, transforms);
      } else if (opticalFlow)
        opticalChains[e]->record(cmd, input[e][old].view,
                                 input[e][current].view, output[e].view,
                                 current, reuse);
      else {
        chains[e]->SetTarget(lsfgDevice, 1, 0, 0, output[e].view);
        chains[e]->DispatchGeneration(cmd, frameCount, 1, 0, 0, output[e].image,
                                      {(uint32_t)width, (uint32_t)height});
      }

      barriers({barrier(output[e].image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, 0,
                        sharedContext ? VK_QUEUE_FAMILY_IGNORED : family,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT)},
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (!fused)
        for (auto &src : input[e]) {
          barriers({barrier(src.image, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                            0, sharedContext ? VK_QUEUE_FAMILY_IGNORED : family,
                  sharedContext ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT)},
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
    }
    if (timestampPool)
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          timestampPool, 2);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS ||
        vkResetFences(device, 1, &fence) != VK_SUCCESS)
      return false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = directActive ? &captureReady : &glReady;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &computeReady;
    completionNeedsDecision = false;
    generationTiming.beginJob({directActive ? readFds[2] : -1,
                               fused          ? (deferredCapture ? -1 : aheadFd)
                               : directActive ? -1
                                              : inputDiagnosticFd,
                               -1});
    submittedAt = generationTiming.submitted;
    preparation.add(submittedAt - prepareStart);
    VkPipelineStageFlags stages[5];
    std::fill_n(stages, 5, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo batches[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, si};
    batches[0].waitSemaphoreCount = waitCount;
    batches[0].pWaitSemaphores = waits.data();
    batches[0].pWaitDstStageMask = stages;
    batches[0].commandBufferCount = 1;
    batches[0].pCommandBuffers = &captureCmd;
    batches[0].signalSemaphoreCount = 1;
    batches[0].pSignalSemaphores = &captureReady;
    // The alignment fence is exported for diagnostics. A SYNC_FD
    // export transfers that payload; use same-queue barriers, not a semaphore
    // wait on the exported payload, between the two batches.
    if (directActive) {
      batches[1].waitSemaphoreCount = outputDependency ? 1 : 0;
      batches[1].pWaitSemaphores = &reuseReady[2];
    }
    // Fused flow samples queue-owned raw history directly. No alignment
    // batch, foreign ownership transfer or diagnostic semaphore is needed.
    if (fused) {
      si.waitSemaphoreCount = outputDependency ? 1 : 0;
      si.pWaitSemaphores = &reuseReady[2];
      stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      inputDiagnosticFd = aheadFd >= 0 ? dup(aheadFd) : -1;
    }
    const bool paired = deferredCapture;
    if (paired) { batches[0] = captureBatch(); batches[1] = si; }
    VkResult submitted = sharedContext ? sharedContext->submit(paired ? 2 : 1, paired ? batches : &si, fence)
                             : directActive && !fused
                             ? vkQueueSubmit(queue, 2, batches, fence)
                             : vkQueueSubmit(queue, 1, &si, fence);
    queueSubmit.add(monotonicNs() - submittedAt);
    if (submitted != VK_SUCCESS)
      return false;
    if (paired) {
      deferredCapture = false;
      if (!exportCapture(captureReleaseFd)) return false;
      if (inputDiagnosticFd >= 0) close(inputDiagnosticFd);
      inputDiagnosticFd = aheadFd >= 0 ? dup(aheadFd) : -1;
    }
    // Export while pending to preserve a sync-file with a signal timestamp.
    // Reuse this exact fence for the existing GLES completion dependency.
    VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    fdInfo.semaphore = computeReady;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int exported = -1;
    if (exportFd(device, &fdInfo, &exported) == VK_SUCCESS)
      completionFd = exported;
    if (directActive && !fused) {
      fdInfo.semaphore = captureReady;
      int captureFd = -1;
      if (exportFd(device, &fdInfo, &captureFd) != VK_SUCCESS) {
        // Export failure: synchronous recovery
        // only, never routine scheduling. Do not reuse an unexported
        // binary semaphore payload on another job.
        { if(sharedContext) sharedContext->idle(); else vkQueueWaitIdle(queue); }
        return false;
      } else if (captureFd >= 0) {
        inputDiagnosticFd = ::dup(captureFd);
        close(captureFd);
      }
      if (!loggedDirect) {
        FGLOG("input path: Vulkan AHB capture + alignment on LSFG queue; early "
              "guest release");
        loggedDirect = true;
      }
    }
    if (fused && !loggedDirect) {
      FGLOG("input path: fused raw-history alignment/luma; private Vulkan "
            "intermediates; one generation batch");
      loggedDirect = true;
    }
    if (onWorker)
      ++workerCaptureJobs;
    if (directActive)
      ++vulkanCaptureJobs;
    else
      ++glesCaptureJobs;
    completedSignalTime = completionObservedAt = 0;
    submittedDisplayTime = time;
    lastFencePoll = submittedAt;
    if (reuse)
      ++reusedHistory;
    else
      ++primedHistory;
    inFlight = true;
    resultAvailable = false;
    resultSynthetic = true;
    return true;
  }
  int64_t completedSignalTime = 0, completionObservedAt = 0;
  bool completionNeedsDecision = false;
  TimingSamples signalToDecision;
  void collectStages(int64_t polledAt) {
    const int64_t inputTime = fenceSignalTime(inputDiagnosticFd);
    const int64_t outputTime = fenceSignalTime(completionFd);
    generationTiming.collect(device, timestampBits, timestampPeriod, outputTime,
                             polledAt, calibratedTimestamps);
    if (guestReadiness) {
      int64_t guestTime = 0;
      bool valid = true, implicit = false;
      for (int fd : guestReadiness->fds) {
        // No acquire fence means no explicit producer dependency.
        const auto signaled =
            fd == -1 ? guestReadiness->claimedAt : fenceSignalTime(fd);
        implicit |= fd == -1;
        valid &= signaled > 0 && signaled <= polledAt;
        guestTime = std::max(guestTime, signaled);
      }
      const auto split = inputReadinessSplit(submittedAt, guestTime, inputTime);
      if (valid && inputTime <= polledAt && split[0] >= 0) {
        guestWait.add(split[0]);
        captureAfterGuest.add(split[1]);
        guestAfterClaim.add(
            std::max<int64_t>(0, guestTime - guestReadiness->claimedAt));
        if (implicit)
          ++implicitReadyPairs;
      } else
        ++guestMisses;
      guestReadiness.reset();
    } else
      ++guestMisses;
    completedSignalTime = outputTime;
    completionNeedsDecision = true;
    if (inputTime > 0 && inputTime <= polledAt)
      inputWait.add(std::max<int64_t>(0, inputTime - submittedAt));
    if (outputTime >= submittedAt && outputTime <= polledAt) {
      signalToPoll.add(polledAt - outputTime);
      if (inputTime > 0 && inputTime <= outputTime)
        afterInput.add(outputTime - std::max(inputTime, submittedAt));
    }
    if (inputDiagnosticFd >= 0)
      close(inputDiagnosticFd);
    inputDiagnosticFd = -1;
    if (timestampPool) {
      uint64_t ticks[3]{};
      // Fence is signaled; never use QUERY_RESULT_WAIT_BIT.
      if (vkGetQueryPoolResults(device, timestampPool, 0, 3, sizeof(ticks),
                                ticks, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        int64_t duration = gpuTimestampDuration(ticks[1], ticks[2],
                                                timestampBits, timestampPeriod);
        if (duration >= 0) {
          gpuElapsed.add(duration);
          vrPerformance.stage(VrPerformanceMetrics::Generation, duration);
        }
        else
          ++timestampMisses;
      } else
        ++timestampMisses;
    }
  }
  bool waitForGlFd(int fd) {
    if(sharedContext) { if(fd>=0) close(fd); return true; }
    if (fd < 0)
      return true; // SYNC_FD -1 represents an already-signaled fence.
    auto create =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto wait = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    auto destroy =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    if (!create || !wait || !destroy) {
      close(fd);
      return false;
    }
    EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
      close(fd);
      return false;
    }
    bool ok = wait(display, sync, 0) == EGL_TRUE;
    destroy(display, sync);
    return ok;
  }
  void publishCompleted() {
    if (pending.valid || presentationLease || inFlight || discardJob ||
        !resultAvailable || !fdReady(completionFd))
      return;
    pending.publish(current);
    pending.endpoint = !resultSynthetic;
    pending.decision = true;
    pending.real = meta[current];
    pending.synthetic = syntheticMeta;
    pending.submitted = submittedAt;
    pending.completed =
        completedSignalTime > 0 ? completedSignalTime : completionObservedAt;
    pending.displayTime = submittedDisplayTime;
    pending.due = jobDue;
    pending.fence = completionFd;
    completionFd = -2;
    if (resultSynthetic) {
      for (int e = 0; e < 2; ++e)
        std::swap(output[e], presentedOutput[e]);
      std::swap(readFds[2], presentedReadFd);
    }
    completionNeedsDecision = false;
    resultAvailable = false;
  }
  void refreshPresentation(int64_t displayTime, int64_t period) {
    if (presentationLease)
      return;
    // Finish an already-started pair before a newer midpoint can replace it.
    // Unstarted pairs and endpoints made obsolete by a long stall may still
    // be replaced, without creating a FIFO of delayed game frames.
    if (pending.valid && resultAvailable && !inFlight &&
        fdReady(completionFd) &&
        mayReplacePresentation(pending.due, pending.endpoint, displayTime, period) &&
        meta[current][0].targetDisplayTime >
            pending.real[0].targetDisplayTime &&
        duePresentation(jobDue, resultSynthetic, displayTime) !=
            PresentationChoice::None) {
      if (!pending.endpoint)
        ++late;
      clearPending();
    }
    if (pending.valid &&
        pending.real[0].targetDisplayTime < lastPresentedTime) {
      ++stale;
      clearPending();
    }
    publishCompleted();
  }
  PresentationChoice pendingChoice(int64_t displayTime, int64_t period) const {
    if (!pending.valid)
      return PresentationChoice::None;
    return duePresentation(
        pending.due,
        !pending.endpoint &&
            freshSynthetic(pending.synthetic[0].targetDisplayTime,
                           lastPresentedTime),
        displayTime, period);
  }
  Output pendingOutput(int64_t time, int64_t displayTime, int64_t period) {
    if (pending.valid) {
      // If the midpoint is obsolete, the real endpoint is immediately useful.
      if (!pending.due.real && (pending.endpoint ||
          !freshSynthetic(pending.synthetic[0].targetDisplayTime, lastPresentedTime)))
        pending.due.spacing = 0;
      PresentationTimeline::selected(pending.due, displayTime);
    }
    const auto choice = pendingChoice(displayTime, period);
    if (choice == PresentationChoice::None)
      return {};
    const auto now = monotonicNs();
    if (pending.decision) {
      displayAge.add(time - pending.displayTime);
      if (pending.completed > 0 && pending.completed <= now)
        signalToDecision.add(now - pending.completed);
      pending.decision = false;
    }
    const bool synthetic = choice == PresentationChoice::Synthetic;
    const auto due = synthetic ? pending.due.midpoint : pending.due.real;
    scheduledHold.add(std::max<int64_t>(0, due - pending.completed));
    deadlineMiss.add(std::max<int64_t>(0, displayTime - due));
    const auto &frame = synthetic ? pending.synthetic : pending.real;
    if (frame[0].targetDisplayTime < lastPresentedTime) {
      ++stale;
      clearPending();
      return {};
    }
    int fd = pending.fence;
    pending.fence = -1;
    if (!waitForGlFd(fd)) {
      fail("presentation fence import failed");
      return {};
    }
    presentationLease = true;
    if (synthetic) {
      pending.endpoint = true;
      ++generated;
      return {{presentedOutput[0].texture, presentedOutput[1].texture},
              pending.synthetic,
              true};
    }
    if (!pending.endpoint)
      ++late;
    Output result{{real[pending.realSlot][0], real[pending.realSlot][1]},
                  pending.real,
                  false};
    clearPending();
    return result;
  }
  void cleanup() {
    clearImages();
    aheadCapture.reset();
    for (auto sem : aheadGuest)
      if (sem)
        vkDestroySemaphore(device, sem, nullptr);
    if (aheadReady)
      vkDestroySemaphore(device, aheadReady, nullptr);
    if (aheadFence)
      vkDestroyFence(device, aheadFence, nullptr);
    capture.reset();
    shaders.reset();
    for (auto sem : reuseReady)
      if (sem)
        vkDestroySemaphore(device, sem, nullptr);
    for (auto sem : guestReady)
      if (sem)
        vkDestroySemaphore(device, sem, nullptr);
    if (captureReady)
      vkDestroySemaphore(device, captureReady, nullptr);
    if (timestampPool)
      vkDestroyQueryPool(device, timestampPool, nullptr);
    if (captureTiming.pool)
      vkDestroyQueryPool(device, captureTiming.pool, nullptr);
    if (computeReady)
      vkDestroySemaphore(device, computeReady, nullptr);
    if (glReady)
      vkDestroySemaphore(device, glReady, nullptr);
    if (fence)
      vkDestroyFence(device, fence, nullptr);
    if (pool)
      vkDestroyCommandPool(device, pool, nullptr);
    if (device && !sharedContext)
      vkDestroyDevice(device, nullptr);
    if (instance && !sharedContext)
      vkDestroyInstance(instance, nullptr);
    if (shader)
      glDeleteProgram(shader);
    if (fbo)
      glDeleteFramebuffers(1, &fbo);
    if (vao)
      glDeleteVertexArrays(1, &vao);
  }
};
VrFrameGenerator::GuestReadiness::GuestReadiness(
    const std::array<EyeFrame, 2> &frames)
    : claimedAt(monotonicNs()) {
  for (int eye = 0; eye < 2; ++eye) {
    if (frames[eye].acquireFenceFd >= 0) {
      fds[eye] = ::dup(frames[eye].acquireFenceFd);
      if (fds[eye] < 0)
        fds[eye] = -2;
    }
  }
}
VrFrameGenerator::GuestReadiness::~GuestReadiness() {
  for (int fd : fds)
    if (fd >= 0)
      close(fd);
}
VrFrameGenerator::VrFrameGenerator() : impl_(std::make_unique<Impl>()) {
  wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}
void VrFrameGenerator::notifyWorker() {
  if (wakeFd_ >= 0) {
    uint64_t one = 1;
    (void)write(wakeFd_, &one, sizeof(one));
  }
}
VrFrameGenerator::~VrFrameGenerator() {
  stopWorker();
  if (wakeFd_ >= 0)
    close(wakeFd_);
}
void VrFrameGenerator::setVulkanContext(XrVulkanContext* context) {
  sharedContext_=context;
  impl_->sharedContext=context;
}
bool VrFrameGenerator::canIngest() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return enabled() && (!impl_->aheadPending || impl_->usingAhead) &&
         !impl_->presentationLease && !impl_->resultAvailable &&
         !impl_->inFlight && impl_->pending.permitsGeneration(impl_->current);
}
bool VrFrameGenerator::canPresentReal() const { return !enabled(); }
bool VrFrameGenerator::ingestVulkan(
    EGLDisplay display, const std::array<EyeFrame, 2> &frames, int64_t time,
    int64_t period, const std::shared_ptr<GuestReadiness> &readiness,
    int &releaseFence) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto &p = *impl_;
  releaseFence = -1;
  if (!enabled())
    return false;
  if (p.sharedContext && (!fdReady(frames[0].acquireFenceFd) || !fdReady(frames[1].acquireFenceFd))) {
    pendingFrames_=frames; pendingGuest_=true;
    releaseFence=kDeferredGuestFence;
    notifyWorker();
    return true; // Retain the guest lease; release only after actual capture.
  }
  const int width =
      frames[0].sourceWidth > 0 ? frames[0].sourceWidth : frames[0].width;
  const int height =
      frames[0].sourceHeight > 0 ? frames[0].sourceHeight : frames[0].height;
  if (!p.device || !p.width || p.width != width || p.height != height) {
    if (p.onWorker || !canIngest() || p.pending.valid)
      return false;
    p.directActive = true;
    ingest(display, {}, frames, time, period, readiness);
    p.directActive = false;
    releaseFence = p.captureReleaseFd;
    p.captureReleaseFd = -1;
    if (p.aheadPending)
      notifyWorker();
    return p.aheadPending;
  }
  if (!p.directStorage || p.aheadPending || !p.retireCapture() || !fdReady(p.captureRealReadFd))
    return false;
  for (const auto &f : frames)
    if ((f.sourceWidth > 0 ? f.sourceWidth : f.width) != p.width ||
        (f.sourceHeight > 0 ? f.sourceHeight : f.height) != p.height)
      return false;
  // Reserve both history and output before recording a two-batch submission.
  // The GPU barrier in submit() connects capture writes to optical-flow reads;
  // there is no host wait between these batches.
  const bool paired = p.sharedContext && canIngest() &&
      fdReady(p.readFds[2]) && p.compatiblePair(frames, period);
  const bool captured = p.captureAhead(frames, releaseFence, paired);
  if (captured && paired) {
    p.usingAhead = p.directActive = true;
    ingest(display, {}, p.aheadFrames, time, period, p.aheadReadiness);
    p.usingAhead = p.directActive = false;
    p.aheadPending = p.capturedReady = false;
    p.aheadReadiness.reset();
    releaseFence = p.captureReleaseFd;
    p.captureReleaseFd = -1;
    if (p.deferredCapture) {
      p.deferredCapture = false;
      p.fail("paired capture/generation submission failed");
    }
  }
  if (captured && !p.onWorker)
    notifyWorker();
  return captured;
}
void VrFrameGenerator::finishRead(const std::array<GLuint, 2> &textures,
                                  int fence) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto &p = *impl_;
  int slot = -1;
  for (int i = 0; i < 2; ++i)
    if (textures[0] && textures[0] == p.real[i][0])
      slot = i;
  if (textures[0] && textures[0] == p.output[0].texture)
    slot = 2;
  if (textures[0] && textures[0] == p.presentedOutput[0].texture) {
    if (p.presentedReadFd >= 0)
      close(p.presentedReadFd);
    p.presentedReadFd = fence;
  } else if (slot >= 0) {
    if (p.readFds[slot] >= 0)
      close(p.readFds[slot]);
    p.readFds[slot] = fence;
  } else if (fence >= 0)
    close(fence);
  p.presentationLease = false;
  notifyWorker();
}
void VrFrameGenerator::cancelPresentation() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  impl_->presentationLease = false;
  notifyWorker();
}
bool VrFrameGenerator::claimFrames(WindowsFrameTransport &transport,
                                   std::array<EyeFrame, 2> &frames) {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return false;
  if (pendingGuest_ && !impl_->pending.valid && canIngest() &&
      (!sharedContext_ || (fdReady(pendingFrames_[0].acquireFenceFd) && fdReady(pendingFrames_[1].acquireFenceFd)))) {
    frames = pendingFrames_;
    pendingGuest_ = false;
    return true;
  }
  if (impl_->directStorage && impl_->width && worker_.joinable() &&
      !pendingGuest_)
    return false;
  // XR-thread fallback capture can replace history/extent. Drain pending
  // presentation first; the worker can still submit compatible Vulkan pairs.
  if (impl_->pending.valid)
    return false;
  if (claimed_ || !canIngest() || !transport.pollStereo(frames))
    return false;
  claimed_ = true;
  return true;
}
bool VrFrameGenerator::claimPassthrough(WindowsFrameTransport &transport,
                                        std::array<EyeFrame, 2> &frames, bool readyOnly) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (pendingGuest_) {
    if (readyOnly && (!fdReady(pendingFrames_[0].acquireFenceFd) ||
                      !fdReady(pendingFrames_[1].acquireFenceFd))) return false;
    frames = pendingFrames_;
    pendingGuest_ = false;
    return true;
  }
  if (claimed_ || !canPresentReal() || !transport.pollStereo(frames, readyOnly))
    return false;
  claimed_ = true;
  return true;
}
void VrFrameGenerator::cancelCapture() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  claimed_ = false;
  notifyWorker();
}
void VrFrameGenerator::releaseGuest(WindowsFrameTransport &transport,
                                    const std::array<EyeFrame, 2> &frames,
                                    int fence) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (fence == kDeferredGuestFence) return;
  int other = fence >= 0 ? ::dup(fence) : -1;
  if (fence >= 0 && other < 0) {
    if (sharedContext_) sharedContext_->idle(); else vkQueueWaitIdle(impl_->queue);
    close(fence);
    fence = -1;
  }
  for (int e = 0; e < 2; ++e) {
    if (frames[e].acquireFenceFd >= 0)
      close(frames[e].acquireFenceFd);
    transport.publishReleaseFence(e, frames[e].imageIndex, e ? fence : other);
  }
  claimed_ = false;
}
void VrFrameGenerator::updateWorker(WindowsFrameTransport &transport,
                                    int64_t time, int64_t period) {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  workerTime_ = time;
  workerPeriod_ = period;
  if (!enabled())
    return;
  if (worker_.joinable()) {
    notifyWorker();
    return;
  }
  transport_ = &transport;
  stopping_ = false;
  if (wakeFd_ < 0 || !transport.setFrameWakeFd(wakeFd_)) {
    impl_->fail("worker notification allocation failed");
    return;
  }
  worker_ = std::thread([this] {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    FGLOG("frame worker: transport/completion events; private history; dynamic "
          "presentation timeline");
    while (!stopping_) {
      auto &p = *impl_;
      p.onWorker = true;
      pollCompletion();
      p.retireCapture();
      if (p.aheadPending && p.capturedReady && p.discardCapture) {
        p.aheadPending = p.capturedReady = p.discardCapture = false;
        if (p.aheadFd >= 0)
          close(p.aheadFd);
        p.aheadFd = -1;
        p.aheadReadiness.reset();
      }
      // A submitted capture is sufficient on the shared queue: generation's
      // barrier waits on the GPU, without requiring a CPU completion round trip.
      // Seed/discontinuous frames still need completion before publication.
      if (enabled() && p.aheadPending &&
          (p.capturedReady || (p.sharedContext && p.captureInFlight &&
                               p.compatiblePair(p.aheadFrames, workerPeriod_))) && !p.inFlight &&
          !p.resultAvailable && !p.presentationLease &&
          p.pending.permitsGeneration(p.current) && fdReady(p.readFds[2])) {
        p.usingAhead = p.directActive = true;
        ingest(p.display, {}, p.aheadFrames, workerTime_, workerPeriod_,
               p.aheadReadiness);
        p.directActive = p.usingAhead = false;
        p.aheadPending = p.capturedReady = false;
        p.aheadReadiness.reset();
        if (p.aheadFd >= 0)
          close(p.aheadFd);
        p.aheadFd = -1;
        // Alignment export is retained only for stage diagnostics, never guest
        // ownership.
        if (p.captureReleaseFd >= 0)
          close(p.captureReleaseFd);
        p.captureReleaseFd = -1;
        p.publishCompleted();
      }
      if (enabled() && pendingGuest_ && p.sharedContext && p.width &&
          !p.aheadPending && !p.captureInFlight && fdReady(p.captureRealReadFd) &&
          fdReady(pendingFrames_[0].acquireFenceFd) && fdReady(pendingFrames_[1].acquireFenceFd)) {
        int fd=-1;
        if (ingestVulkan(p.display,pendingFrames_,workerTime_,workerPeriod_,{},fd)) {
          pendingGuest_=false;
          releaseGuest(*transport_,pendingFrames_,fd);
        }
      }
      // A free capture slot can be filled while the previous generation runs.
      if (enabled() && p.directStorage && p.width && !claimed_ &&
          !p.aheadPending && !p.captureInFlight && fdReady(p.captureRealReadFd)) {
        std::array<EyeFrame, 2> frames{};
        if (transport_->pollStereo(frames)) {
          claimed_ = true;
          int fd = -1;
          if (ingestVulkan(p.display, frames, workerTime_, workerPeriod_, {},
                           fd))
            releaseGuest(*transport_, frames, fd);
          else {
            pendingFrames_ = frames;
            pendingGuest_ = true;
          }
        }
      }
      p.onWorker = false;
      // Duplicates outlive unlock/reset. Poll sleeps on actual sync-file
      // completion and source/publication wakeups, not an XR or 4ms tick.
      pollfd events[8]{};
      nfds_t count = 0;
      if (wakeFd_ >= 0)
        events[count++] = {wakeFd_, POLLIN, 0};
      auto add = [&](int fd) {
        if (fd >= 0 && !fdReady(fd)) {
          int copy = dup(fd);
          if (copy >= 0)
            events[count++] = {copy, POLLIN, 0};
        }
      };
      if (pendingGuest_ && p.sharedContext) {
        add(pendingFrames_[0].acquireFenceFd); add(pendingFrames_[1].acquireFenceFd);
      }
      if (p.inFlight || p.resultAvailable)
        add(p.completionFd);
      if (p.captureInFlight)
        add(p.captureRetireFd);
      if (!p.inFlight)
        add(p.readFds[2]);
      if (!p.aheadPending)
        add(p.captureRealReadFd);
      const bool unexported =
          (p.inFlight && (p.completionFd < 0 || fdReady(p.completionFd))) ||
          (p.captureInFlight && fdReady(p.captureRetireFd));
      lock.unlock();
      int status;
      do {
        status = poll(events, count, unexported ? 1 : -1);
      } while (status < 0 && errno == EINTR);
      bool badFence = status < 0;
      for (nfds_t i = 0; i < count; ++i) {
        badFence |= (events[i].revents & (POLLERR | POLLNVAL)) != 0;
        if (events[i].fd == wakeFd_) {
          uint64_t value;
          while (read(wakeFd_, &value, sizeof(value)) > 0) {
          }
        } else
          close(events[i].fd);
      }
      lock.lock();
      if (badFence && !stopping_)
        p.fail("worker sync-file wait failed");
    }
  });
}
void VrFrameGenerator::stopWorker() {
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stopping_ = true;
    if (transport_)
      transport_->setFrameWakeFd(-1);
    notifyWorker();
  }
  if (worker_.joinable())
    worker_.join();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (pendingGuest_ && transport_) {
    for (int e = 0; e < 2; ++e) {
      if (pendingFrames_[e].acquireFenceFd >= 0)
        close(pendingFrames_[e].acquireFenceFd);
      transport_->discardFrame(e, pendingFrames_[e].imageIndex,
                               pendingFrames_[e].serial);
    }
  }
  pendingGuest_ = claimed_ = false;
  transport_ = nullptr;
}
void VrFrameGenerator::notePresented(int64_t targetTime) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  impl_->lastPresentedTime = std::max(impl_->lastPresentedTime, targetTime);
}
void VrFrameGenerator::reset() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto &p = *impl_;
  if (pendingGuest_ && transport_) {
    for (int e = 0; e < 2; ++e) {
      if (pendingFrames_[e].acquireFenceFd >= 0)
        close(pendingFrames_[e].acquireFenceFd);
      transport_->discardFrame(e, pendingFrames_[e].imageIndex,
                               pendingFrames_[e].serial);
    }
    pendingGuest_ = claimed_ = false;
  }
  // Retire capture asynchronously; its images remain reserved until its fence
  // signals.
  p.discardCapture = p.aheadPending;
  notifyWorker();
  p.presentationLease = false;
  p.clearPending();
  p.history = false;
  p.lastPresentedTime = 0;
  p.completionNeedsDecision = false;
  p.flowHistory.reset();
  p.timeline.reset();
  p.resultAvailable = false;
  p.discardJob = p.inFlight;
}
void VrFrameGenerator::shutdown() {
  stopWorker();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  impl_->cleanup();
  impl_ = std::make_unique<Impl>();
}
void VrFrameGenerator::pollCompletion() {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (!enabled())
    return;
  auto &p = *impl_;
  if (p.inFlight) {
    VkResult status = vkGetFenceStatus(p.device, p.fence);
    const int64_t polledAt = monotonicNs();
    p.pollGap.add(polledAt - p.lastFencePoll);
    p.lastFencePoll = polledAt;
    if (status != VK_SUCCESS && status != VK_NOT_READY) {
      p.fail("generation fence failed");
      return;
    }
    if (status == VK_SUCCESS) {
      p.collectStages(polledAt);
      p.completionObservedAt = polledAt;
      p.completion.add(polledAt - p.submittedAt);
      p.inFlight = false;
      p.resultAvailable = !p.discardJob;
      const auto readyAt =
          p.completedSignalTime > 0 ? p.completedSignalTime : polledAt;
      if (p.jobArrival > 0)
        p.arrivalToResult.add(std::max<int64_t>(0, readyAt - p.jobArrival));
      notifyWorker();
      if (p.completionFd == -2) {
        VkSemaphoreGetFdInfoKHR info{
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        info.semaphore = p.computeReady;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd = -1;
        if (p.exportFd(p.device, &info, &fd) != VK_SUCCESS) {
          p.fail("completion fence export failed");
          return;
        }
        p.completionFd = fd;
      }
      if (p.glReuseFd >= 0)
        close(p.glReuseFd);
      p.glReuseFd = p.completionFd >= 0 ? ::dup(p.completionFd) : -1;
      if (p.completionFd >= 0 && p.glReuseFd < 0) {
        p.fail("GLES reuse fence duplication failed");
        return;
      }
    }
  }
  if (!p.inFlight && p.discardJob) {
    if (p.completionFd >= 0)
      close(p.completionFd);
    p.completionFd = -2;
    p.discardJob = false;
    p.resultAvailable = false;
    p.completionNeedsDecision = false;
  }
  p.publishCompleted();
}
VrFrameGenerator::Output VrFrameGenerator::advance(int64_t time, int64_t period,
                                                   int64_t displayDeadline) {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !enabled())
    return {};
  auto &p = *impl_;
  p.logTimings(monotonicNs(), period);
  const auto now = monotonicNs();
  const auto display = displayDeadline > 0 ? displayDeadline : now;
  // Driver command preparation runs on the worker. XR only selects published,
  // completed images and never waits for it to release the coordination lock.
  p.refreshPresentation(display, period);
  auto result=p.pendingOutput(time, display, period);
  if(p.sharedContext && result) {
    for(int e=0;e<2;++e) {
      if(result.synthetic) { result.images[e]=p.presentedOutput[e].image;result.views[e]=p.presentedOutput[e].view; }
      else for(int i=0;i<2;++i) if(p.real[i][e]==result.textures[e]) {
        result.images[e]=p.realShared[i][e].image;result.views[e]=p.realShared[i][e].view;
      }
    }
  }
  return result;
}
VrFrameGenerator::Output VrFrameGenerator::ingest(
    EGLDisplay display, const std::array<GLuint, 2> &textures,
    const std::array<EyeFrame, 2> &frames, int64_t time, int64_t period,
    const std::shared_ptr<GuestReadiness> &guestReadiness) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto &p = *impl_;
  if (!canIngest())
    return {};
  p.display = display;
  if (!p.directActive && p.glReuseFd >= 0) {
    int fd = p.glReuseFd;
    p.glReuseFd = -1;
    if (!p.waitForGlFd(fd)) {
      p.fail("GLES reuse fence import failed");
      return {};
    }
  }
  if (!p.device && !p.initDevice()) {
    p.fail("Vulkan device/shader capabilities unavailable");
    return {};
  }
  int w = frames[0].sourceWidth > 0 ? frames[0].sourceWidth : frames[0].width;
  int h =
      frames[0].sourceHeight > 0 ? frames[0].sourceHeight : frames[0].height;
  const int rw =
      frames[1].sourceWidth > 0 ? frames[1].sourceWidth : frames[1].width;
  const int rh =
      frames[1].sourceHeight > 0 ? frames[1].sourceHeight : frames[1].height;
  if (w <= 0 || h <= 0 || w != rw || h != rh || w > 4096 || h > 4096) {
    p.fail("unsupported stereo extent");
    return {};
  }
  bool allocated = p.resize(w, h);
  if (!allocated && p.useFp16) {
    // A driver may advertise FP16 but reject a particular pipeline. Keep
    // generation available using the FP32 performance-model cache.
    p.clearImages();
    p.useFp16 = false;
    VkPhysicalDevice16BitStorageFeatures storage{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    if (usableCache(p.cache, false, storage)) {
      p.shaders = std::make_unique<lsfg::LsfgShaders>(p.lsfgDevice, p.cache);
      if (p.shaders->IsValid())
        allocated = p.resize(w, h);
    }
    FGLOG("FP16 pipeline/allocation fallback to FP32: %s",
          allocated ? "ready" : "failed");
  }
  if (!allocated) {
    p.fail("stereo allocation/pipeline creation failed");
    return {};
  }
  if (p.directStorage && !p.usingAhead) {
    if (p.directActive) {
      int fd = -1;
      if (!p.captureAhead(frames, fd))
        return {};
      p.captureReleaseFd = fd;
      return {};
    }
    // Unsupported source import: drain ownership before using the GLES path.
    if(p.sharedContext) { p.fail("native source import unavailable");return {}; }
    p.directStorage = false;
    p.clearImages();
    if (!p.resize(w, h)) {
      p.fail("GLES fallback allocation failed");
      return {};
    }
  }
  if (p.directActive && !p.directStorage)
    return {}; // XR caller supplies GLES fallback textures next
  const int old = p.current;
  p.current = 1 - p.current;
  if (p.usingAhead) {
    // Capture owns a third real-image pair. Move ownership, not pixels, only
    // once the destination history slot is no longer retained by presentation.
    // Its prior GLES read fence follows the image back to the capture bank.
    for (int eye = 0; eye < 2; ++eye) {
      if (!p.sharedContext) std::swap(p.rawHistory[p.current][eye], p.aheadImages[eye]);
      std::swap(p.realShared[p.current][eye], p.captureRealShared[eye]);
      p.real[p.current][eye] = p.realShared[p.current][eye].texture;
    }
    std::swap(p.readFds[p.current], p.captureRealReadFd);
  }
  bool interpolate =
      p.history && interpolationInterval(p.meta[old][0].targetDisplayTime,
                                         frames[0].targetDisplayTime, period);
  for (int e = 0; e < 2; ++e) {
    p.meta[p.current][e] = metadata(frames[e], w, h);
    interpolate &= frames[e].projectionValid && p.meta[old][e].projectionValid;
    float dot = 0, dist = 0;
    for (int i = 0; i < 4; ++i)
      dot += frames[e].projectionOrientation[i] *
             p.meta[old][e].projectionOrientation[i];
    for (int i = 0; i < 3; ++i) {
      float d = frames[e].projectionPosition[i] -
                p.meta[old][e].projectionPosition[i];
      dist += d * d;
    }
    // Discontinuous tracking/teleport or a large FOV change: use a real frame.
    interpolate &= std::abs(dot) > .97f && dist < .04f;
    for (int i = 0; i < 4; ++i)
      interpolate &= std::abs(frames[e].projectionFov[i] -
                              p.meta[old][e].projectionFov[i]) < .02f;
  }
  p.jobArrival = std::max(frames[0].receivedAt, frames[1].receivedAt);
  if (!p.jobArrival)
    p.jobArrival = monotonicNs();
  p.jobDue = p.timeline.begin(p.jobArrival, period, interpolate,
                              frames[0].arrivalInterval);
  p.history = true;
  if (!interpolate) {
    if (p.usingAhead) {
      p.flowHistory.reset();
      p.resultAvailable = true;
      p.resultSynthetic = false;
      p.submittedAt = p.jobArrival;
      p.submittedDisplayTime = time;
      p.completedSignalTime = p.completionObservedAt = monotonicNs();
      if (p.completionFd >= 0)
        close(p.completionFd);
      p.completionFd = p.aheadFd >= 0 ? dup(p.aheadFd) : -1;
      if (p.aheadFd >= 0 && p.completionFd < 0)
        p.fail("capture completion duplication failed");
      return {};
    }
    for (int e = 0; e < 2; ++e) {
      if (!p.draw(textures[e], p.real[p.current][e], frames[e], nullptr)) {
        p.fail("eye capture failed");
        return {};
      }
    }
    auto create =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    auto duplicate = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress(
        "eglDupNativeFenceFDANDROID");
    auto destroy =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    int writeFence = -1;
    if (create && duplicate && destroy) {
      const EGLint attrs[] = {EGL_NONE};
      auto sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
      if (sync != EGL_NO_SYNC_KHR) {
        glFlush();
        writeFence = duplicate(display, sync);
        destroy(display, sync);
      }
    }
    if (writeFence < 0)
      glFinish();
    if (p.readFds[p.current] >= 0)
      close(p.readFds[p.current]);
    p.readFds[p.current] = writeFence;
    p.flowHistory.reset();
    p.resultAvailable = true;
    p.resultSynthetic = false;
    p.submittedAt = p.jobArrival;
    p.submittedDisplayTime = time;
    p.completedSignalTime = p.completionObservedAt = monotonicNs();
    if (p.completionFd >= 0)
      close(p.completionFd);
    p.completionFd = writeFence >= 0 ? dup(writeFence) : -1;
    if (writeFence >= 0 && p.completionFd < 0) {
      p.fail("GLES seed fence duplication failed");
      return {};
    }
    p.publishCompleted();
    notifyWorker();
    return {};
  }
  if (!p.submit(time, textures, frames)) {
    p.fail("stereo compute submission failed");
    return {};
  }
  p.guestReadiness = guestReadiness;
  return {};
}
} // namespace xrimmersive::windowsvr
