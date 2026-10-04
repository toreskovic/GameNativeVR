#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <time.h>

namespace xrimmersive::windowsvr {
// Bound RAW -> MONOTONIC offset with a sandwich, then retain the envelope
// across submission and completion. This includes observed drift and sampling
// jitter; it is not a guarantee against an unobserved clock discontinuity.
struct RawClockBridge {
    int64_t lower = 0, upper = 0;
    bool valid = false;
    static RawClockBridge bracket(int64_t before, int64_t raw, int64_t after) {
        if (before <= 0 || raw <= 0 || after < before || after - before > 1000000)
            return {};
        return {before - raw, after - raw, true};
    }
    void include(const RawClockBridge& other) {
        if (!valid || !other.valid) { valid = false; return; }
        lower = std::min(lower, other.lower);
        upper = std::max(upper, other.upper);
    }
    uint64_t uncertainty() const {
        return valid ? uint64_t((static_cast<__int128>(upper) - lower + 1) / 2) : UINT64_MAX;
    }
    int64_t convert(uint64_t raw) const {
        if (!valid || uncertainty() > 1000000) return -1;
        const __int128 value = static_cast<__int128>(raw) + lower + uncertainty();
        return value > 0 && value < INT64_MAX ? int64_t(value) : -1;
    }
};
inline RawClockBridge sampleRawClockBridge() {
    timespec a{}, r{}, b{};
    if (clock_gettime(CLOCK_MONOTONIC, &a) ||
        clock_gettime(CLOCK_MONOTONIC_RAW, &r) ||
        clock_gettime(CLOCK_MONOTONIC, &b)) return {};
    auto ns = [](const timespec& t) { return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec; };
    return RawClockBridge::bracket(ns(a), ns(r), ns(b));
}
// Partition submission-to-input readiness using only CLOCK_MONOTONIC times.
// The residual is capture + queue/synchronization delay, not isolated GPU work.
inline std::array<int64_t, 2> inputReadinessSplit(int64_t submitted, int64_t guestReady, int64_t inputReady) {
    if (submitted <= 0 || guestReady <= 0 || inputReady <= 0 || guestReady > inputReady)
        return {-1, -1};
    const auto total = std::max<int64_t>(0, inputReady - submitted);
    const auto guest = std::max<int64_t>(0, guestReady - submitted);
    return {guest, total - guest};
}
// Vulkan timestamps may wrap at timestampValidBits; CPU/GPU clocks are never
// subtracted from each other. A full wrap between samples is not recoverable.
inline int64_t gpuTimestampDuration(uint64_t start, uint64_t end, uint32_t bits, double periodNs) {
    if (!bits || bits > 64 || !std::isfinite(periodNs) || periodNs <= 0)
        return -1;
    const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    const double ns = double((end - start) & mask) * periodNs;
    if (!std::isfinite(ns) || ns >= double(std::numeric_limits<int64_t>::max()))
        return -1;
    return static_cast<int64_t>(ns);
}
// Map a nearby device tick onto a calibrated monotonic clock. Reject ambiguous
// half-wraps and old samples rather than silently inventing cross-clock timings.
inline int64_t calibratedGpuTime(uint64_t tick, uint64_t reference, int64_t host,
                                uint32_t bits, double periodNs) {
    if (!bits || bits > 64 || host <= 0 || !std::isfinite(periodNs) || periodNs <= 0)
        return -1;
    const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    const uint64_t half = uint64_t(1) << (bits - 1);
    const uint64_t delta = (tick - reference) & mask;
    if (delta == half) return -1;
    const long double ns = delta < half ? static_cast<long double>(delta) * periodNs
        : -static_cast<long double>((~delta + 1) & mask) * periodNs;
    const long double value = host + ns;
    if (std::abs(ns) > 5000000000.L || value <= 0 || value >= INT64_MAX) return -1;
    return static_cast<int64_t>(value);
}
} // namespace xrimmersive::windowsvr
