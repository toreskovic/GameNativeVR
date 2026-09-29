#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../app/src/main/windows/openxr_runtime/gamenative_xr_clock.h"

int main(void) {
    const long long frequencies[] = {10000000, 19200000, 24000000, 1000000000, 3000000000LL};
    for (unsigned f = 0; f < sizeof(frequencies) / sizeof(*frequencies); ++f) {
        // Deliberately different epochs, well beyond double's exact integer range.
        GnXrClock clock = {123456789012345678LL, 234567890123456789LL, frequencies[f]};
        long long out, back;
        assert(gn_xr_clock_convert(&clock, clock.counter, 1, &out) && out == clock.time);
        assert(gn_xr_clock_convert(&clock, clock.time, 0, &out) && out == clock.counter);
        for (long long seconds = -3600; seconds <= 3600; seconds += 17) {
            long long counter = clock.counter + seconds * clock.frequency + 37;
            assert(gn_xr_clock_convert(&clock, counter, 1, &out));
            long double exactDelta = (long double)(counter - clock.counter) * 1000000000.0L / clock.frequency;
            assert(fabsl((long double)(out - clock.time) - exactDelta) < 1.01L);
            assert(gn_xr_clock_convert(&clock, out, 0, &back));
            assert(llabs(back - counter) <= (clock.frequency / 1000000000LL) + 1);
        }
        assert(!gn_xr_clock_convert(&clock, 0, 1, &out));
        assert(!gn_xr_clock_convert(&clock, -1, 0, &out));
        assert(!gn_xr_clock_convert(&clock, LLONG_MIN, 1, &out));
        assert(!gn_xr_clock_convert(&clock, clock.time, 0, NULL));
    }
    long long out = 42;
    GnXrClock overflow = {1, LLONG_MAX - 10, 10000000};
    assert(!gn_xr_clock_convert(&overflow, 2, 1, &out) && out == 42);
    GnXrClock hugeDelta = {1, 1, 1};
    assert(!gn_xr_clock_convert(&hugeDelta, LLONG_MAX, 1, &out));
    GnXrClock negative = {10000000, 1, 10000000};
    assert(!gn_xr_clock_convert(&negative, 1, 1, &out));
    puts("XR clock conversion: epochs, five frequencies, round trips, invalid inputs and overflow passed");
}
