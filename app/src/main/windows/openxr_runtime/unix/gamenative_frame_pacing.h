#pragma once
#include <stdint.h>

/* Host CLOCK_MONOTONIC durations only. XrTime is a matching key, never an input
 * to duration arithmetic. Ignore pauses/loading outliers; smooth normal jitter
 * rather than changing the prediction horizon on every frame. */
struct gn_production_estimate { uint64_t mean_ns, deviation_ns; };
static inline void gn_production_observe(struct gn_production_estimate *e, uint64_t duration_ns)
{
    if (duration_ns < 1000000ull || duration_ns > 200000000ull) return;
    if (!e->mean_ns) { e->mean_ns = duration_ns; e->deviation_ns = 0; return; }
    const uint64_t delta = duration_ns > e->mean_ns ? duration_ns - e->mean_ns : e->mean_ns - duration_ns;
    e->deviation_ns = (e->deviation_ns * 7 + delta) / 8;
    e->mean_ns = (e->mean_ns * 7 + duration_ns) / 8;
}
static inline uint64_t gn_production_budget(const struct gn_production_estimate *e)
{
    uint64_t budget = e->mean_ns + e->deviation_ns;
    return budget > 100000000ull ? 100000000ull : budget;
}
