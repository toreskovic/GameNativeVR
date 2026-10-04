#include "../../app/src/main/cpp/xrimmersive/xr_frame_pacing.h"
#include "../../app/src/main/windows/openxr_runtime/unix/gamenative_frame_pacing.h"
#include <cassert>
#include <cstdio>
int main() {
    using xrimmersive::guestDisplayTarget;
    constexpr int64_t base = 1000000000;
    // 72/90/120 Hz, fast/slow work, changing refresh, and monotonic targets.
    int64_t previous = 0;
    for (int64_t period : {13888889LL, 11111111LL, 8333333LL}) {
        for (int64_t budget : {0LL, 5000000LL, 27000000LL, 45000000LL}) {
            const int64_t now = base + 7000000;
            const auto target = guestDisplayTarget(base, period, now, budget, previous);
            assert(target > previous && (target - base) % period == 0);
            assert(target >= now + (budget ? budget : period) + period);
            previous = target;
        }
    }
    assert(guestDisplayTarget(base,11111111,0,30000000,0)==base); // no mixed clocks
    assert(guestDisplayTarget(base,0,base,30000000,0)==base);
    const auto capped = guestDisplayTarget(base,11111111,base,INT64_MAX,0);
    assert(capped < base + 125000000);
    // A completed frame keeps its original target: selection only applies to
    // newly admitted work. No sleep/cadence accumulator in this policy.
    gn_production_estimate estimate{};
    assert(gn_production_budget(&estimate)==0);
    gn_production_observe(&estimate,40000000);
    assert(gn_production_budget(&estimate)==40000000);
    gn_production_observe(&estimate,20000000);
    assert(estimate.mean_ns==37500000 && gn_production_budget(&estimate)==40000000);
    gn_production_observe(&estimate,1000000000); // paused/loading frame
    assert(gn_production_budget(&estimate)==40000000);
    for(int i=0;i<80;++i) gn_production_observe(&estimate,10000000);
    assert(gn_production_budget(&estimate)<11000000); // adapts down without permanent high-water mark
    for(int i=0;i<80;++i) gn_production_observe(&estimate,180000000);
    assert(gn_production_budget(&estimate)==100000000);
    puts("Runtime slot alignment, refresh changes, monotonic targets, clock failure, budget smoothing/outlier rejection passed");
}
