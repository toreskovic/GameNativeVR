#ifndef GAMENATIVE_XR_CLOCK_H
#define GAMENATIVE_XR_CLOCK_H

typedef struct GnXrClock {
    long long counter;
    long long time;
    long long frequency;
} GnXrClock;

// Subtract integer anchors before scaling, retaining precision for nearby timestamps.
// Positive signed inputs make the subtraction safe. Check the exclusive upper bound
// before converting floating point to integer (LLONG_MAX rounds up as a double).
static int gn_xr_clock_convert(const GnXrClock* clock, long long value, int to_time, long long* out) {
    if (!out || value <= 0 || clock->frequency <= 0 || clock->counter <= 0 || clock->time <= 0) return 0;
    long long origin = to_time ? clock->counter : clock->time;
    long long target = to_time ? clock->time : clock->counter;
    double scale = to_time ? 1000000000.0 / (double)clock->frequency : (double)clock->frequency / 1000000000.0;
    double delta = (double)(value - origin) * scale;
    if (!(delta > -9223372036854775808.0 && delta < 9223372036854775808.0)) return 0;
    long long ticks = (long long)delta;
    if (ticks > 0 && target > 0x7fffffffffffffffLL - ticks) return 0;
    if (ticks <= -target) return 0;
    *out = target + ticks;
    return 1;
}
#endif
