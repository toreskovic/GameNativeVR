#pragma once
#include <algorithm>
#include <cstdint>

namespace xrimmersive {
// Select a future slot on the *runtime's* timeline. This predicts when newly
// started game work can be displayed; it does not relabel an already rendered
// image, sleep until that slot, or impose a fractional-refresh FPS cap.
inline int64_t guestDisplayTarget(int64_t anchor, int64_t period, int64_t now,
                                  int64_t productionBudget, int64_t previous) {
    if (anchor <= 0 || now <= 0 || period < 4000000 || period > 50000000)
        return anchor;
    const int64_t budget = productionBudget > 0 ?
        std::clamp<int64_t>(productionBudget, 1000000, 100000000) : period;
    // Reserve one runtime interval for selection/presentation after the copy.
    // Only short, measured budgets are accepted; pause/loading outliers must not
    // turn into hundreds of milliseconds of head/controller prediction.
    const int64_t earliest = std::max(now + budget + period, previous + 1);
    if (earliest <= anchor) return anchor;
    return anchor + ((earliest - anchor + period - 1) / period) * period;
}
} // namespace xrimmersive
