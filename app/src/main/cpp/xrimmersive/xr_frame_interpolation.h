#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace xrimmersive::windowsvr {
// Quaternion order follows OpenXR: x, y, z, w. Matrices are column-major for GL.
inline std::array<float, 4> midpointRotation(const float *a, const float *b) {
    float dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += a[i] * b[i];
    std::array<float, 4> q{};
    float length = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = a[i] + (dot < 0 ? -b[i] : b[i]);
        length += q[i] * q[i];
    }
    if (!std::isfinite(length) || length < 1e-8f)
        return {0, 0, 0, 1};
    for (auto &v : q)
        v /= std::sqrt(length);
    return q;
}
inline std::array<float, 9> rotationMatrix(const float *q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
            2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
            2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
}
inline std::array<float, 9> targetToSourceRotation(const float *source, const float *target) {
    const auto a = rotationMatrix(source), b = rotationMatrix(target);
    std::array<float, 9> m{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                m[c * 3 + r] += a[r * 3 + k] * b[c * 3 + k];
    return m;
}
// Three padded columns mapping anchor UV to homogeneous raw-image UV.
// 48 bytes per transform keeps two transforms plus sizes below Vulkan's
// guaranteed 128-byte push-constant limit. Raw snapshots are already cropped/flipped.
struct ColorTransform {
    std::array<float, 12> columns{1,0,0,0, 0,1,0,0, 0,0,1,0};
};
inline ColorTransform colorTransform(const float* sourceOrientation, const float* sourceFov,
                                     const float* targetOrientation, const float* targetFov) {
    const auto r = targetToSourceRotation(sourceOrientation, targetOrientation);
    const float sl=std::tan(sourceFov[0]), sd=std::tan(sourceFov[2]);
    const float sw=std::tan(sourceFov[1])-sl, sh=std::tan(sourceFov[3])-sd;
    const float tl=std::tan(targetFov[0]), td=std::tan(targetFov[2]);
    const float tw=std::tan(targetFov[1])-tl, th=std::tan(targetFov[3])-td;
    ColorTransform result;
    for (int c=0;c<3;++c) {
        float ray[3];
        for (int row=0;row<3;++row)
            ray[row]=c==0 ? r[row]*tw : c==1 ? r[3+row]*th : r[row]*tl+r[3+row]*td-r[6+row];
        const float z=-ray[2];
        result.columns[c*4]=(ray[0]-sl*z)/sw;
        result.columns[c*4+1]=(ray[1]-sd*z)/sh;
        result.columns[c*4+2]=z;
        result.columns[c*4+3]=0;
    }
    return result;
}
// Keep a short-lived fixed rotation/FOV reference so optical-flow features
// remain in the same coordinates. Bound edge clamping to a five-degree turn.
inline bool compatibleFlowAnchor(const float *orientation, const float *fov, const float *anchorOrientation,
                                 const float *anchorFov) {
    float dot = 0, normA = 0, normB = 0;
    for (int i = 0; i < 4; ++i) {
        dot += orientation[i] * anchorOrientation[i];
        normA += orientation[i] * orientation[i];
        normB += anchorOrientation[i] * anchorOrientation[i];
        if (!std::isfinite(fov[i]) || !std::isfinite(anchorFov[i]) || std::abs(fov[i] - anchorFov[i]) > .002f)
            return false;
    }
    return std::isfinite(dot) && normA > 1e-8f && normB > 1e-8f &&
           dot * dot >= .99809735f * normA * normB; // cos(2.5 degrees)^2
}

// Counts advance for each processed source frame, including across the 2-image
// and 3-feature-history rings. A new anchor primes both endpoints exactly once.
class FlowHistory {
  public:
    void reset() { valid_ = false; }
    bool matches(int64_t previousTime) const { return valid_ && time_ == previousTime; }
    uint64_t begin(bool reuse, int oldSlot, int64_t currentTime) {
        count_ = reuse ? count_ + 1 : uint64_t(oldSlot) + 1;
        time_ = currentTime;
        valid_ = true;
        return count_;
    }

  private:
    bool valid_ = false;
    int64_t time_ = 0;
    uint64_t count_ = 0;
};

inline int64_t nominalDisplayPeriod(int64_t runtimePeriod, int64_t previousPeriod) {
    return runtimePeriod >= 1000000 && runtimePeriod <= 50000000 ? runtimePeriod : previousPeriod;
}
inline bool freshSynthetic(int64_t midpointTime, int64_t lastPresentedTime) {
    return midpointTime > lastPresentedTime;
}

inline bool interpolationInterval(int64_t previous, int64_t current, int64_t period) {
    // No history across pauses, non-monotonic timestamps, or near-native cadence.
    const int64_t delta = current - previous;
    return previous > 0 && period > 0 && delta >= period * 3 / 2 && delta <= 100000000;
}

// Presentation owns a real-history slot until its endpoint is consumed.
// A second job may write the other slot, but a third must not overwrite it.
struct BufferedResultState {
    bool valid = false, endpoint = false;
    int realSlot = 0;
    bool permitsGeneration(int currentHistory) const { return !valid || realSlot != 1 - currentHistory; }
    void publish(int slot) {
        valid = true;
        endpoint = false;
        realSlot = slot;
    }
};

// Updated on complete stereo arrival, even when the consumer skips a frame.
class StereoArrivalCadence {
  public:
    int64_t observe(uint64_t frame, int64_t arrival) {
        if (!frame || frame == frame_) return interval_;
        const auto delta = arrival - arrival_;
        if (!frame_ || frame < frame_ || delta <= 0 || delta > 500000000)
            interval_ = 0;
        else
            interval_ = interval_ ? (interval_ * 3 + delta) / 4 : delta;
        frame_ = frame;
        arrival_ = arrival;
        return interval_;
    }
  private:
    uint64_t frame_ = 0;
    int64_t arrival_ = 0, interval_ = 0;
};

// Pair spacing follows source arrivals, but deadlines are anchored only when a
// completed result is actually selected. Processing spikes never impose extra
// safety holds on later ready frames. Times here are CPU-monotonic display times,
// separate from the image's rendering pose timestamp.
class PresentationTimeline {
  public:
    struct Pair {
        int64_t midpoint = 0, real = 0, spacing = 0;
    };
    void reset() { *this = {}; }
    Pair begin(int64_t arrival, int64_t period, bool interpolate, int64_t sourceInterval = 0) {
        period = std::max<int64_t>(period, 1000000);
        const auto delta = arrival - lastArrival_;
        if (lastArrival_ && (delta <= 0 || delta > 500000000)) reset();
        if (lastArrival_ && delta > 0 && delta <= 500000000)
            cadence_ = cadence_ ? (cadence_ * 3 + delta) / 4 : delta;
        if (sourceInterval > 0 && sourceInterval <= 500000000)
            cadence_ = sourceInterval;
        lastArrival_ = arrival;
        const auto interval = cadence_ ? cadence_ : period * 2;
        return {0, 0, interpolate ? std::max(period, interval / 2) : 0};
    }
    // Called once, at the first eligible XR selection, never at submission.
    static void selected(Pair &pair, int64_t display) {
        if (pair.real != 0) return;
        pair.midpoint = display;
        pair.real = display + pair.spacing;
    }
    int64_t cadence() const { return cadence_; }
  private:
    int64_t lastArrival_ = 0, cadence_ = 0;
};

enum class PresentationChoice { None, Synthetic, Real };
inline PresentationChoice duePresentation(PresentationTimeline::Pair due, bool midpointRemaining,
                                          int64_t displayTime, int64_t displayPeriod = 0) {
    // An unscheduled completed pair is eligible immediately. No processing-
    // time estimate or extra display period gates its first presentation.
    if (due.real == 0)
        return midpointRemaining ? PresentationChoice::Synthetic : PresentationChoice::Real;
    // Late midpoints expire individually; never postpone a due real endpoint.
    if (displayTime >= due.real)
        return PresentationChoice::Real;
    // Once the midpoint has been selected, use a slot within half a nominal
    // refresh of the endpoint's ideal time instead of repeating until the next
    // slot. Never collapse both images into the same presentation slot.
    if (!midpointRemaining && displayTime > due.midpoint &&
        due.real - displayTime <= std::max<int64_t>(0, displayPeriod) / 2)
        return PresentationChoice::Real;
    if (midpointRemaining && displayTime >= due.midpoint)
        return PresentationChoice::Synthetic;
    return PresentationChoice::None;
}

inline bool mayReplacePresentation(PresentationTimeline::Pair due, bool midpointSelected,
                                   int64_t displayTime, int64_t displayPeriod) {
    // An unstarted pair may be replaced by fresher work. Once its midpoint was
    // selected, reserve the endpoint until it gets a slot, even if a newer
    // pair completes first. A long interruption may still skip obsolete work.
    if (!midpointSelected || due.real == 0 || due.spacing == 0) return true;
    const auto grace = std::max(due.spacing * 2,
                               std::max<int64_t>(displayPeriod, 1000000) * 3);
    return displayTime > due.real && displayTime - due.real > grace;
}
} // namespace xrimmersive::windowsvr
