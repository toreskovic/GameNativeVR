#include "../../app/src/main/cpp/xrimmersive/xr_frame_interpolation.h"
#include "../../app/src/main/cpp/xrimmersive/xr_lsfg_timing.h"
#include <cassert>
#include <iostream>
using namespace xrimmersive::windowsvr;
void near(float a, float b) { assert(std::abs(a - b) < 1e-5f); }
int main() {
    auto bridge = RawClockBridge::bracket(1000, 2000, 1020);
    assert(bridge.valid && bridge.uncertainty() == 10 && bridge.convert(3000) == 2010);
    bridge.include(RawClockBridge::bracket(1030, 2040, 1050));
    assert(bridge.uncertainty() == 15 && bridge.convert(3000) == 2005);
    assert(RawClockBridge::bracket(2000, 1000, 2020).convert(3000) == 4010);
    assert(!RawClockBridge::bracket(1000, 2000, 999).valid);
    assert(!RawClockBridge::bracket(1000, 2000, 1001001).valid);
    assert(bridge.convert(UINT64_MAX) == -1);
    bridge.include(RawClockBridge::bracket(3000000, 1000, 3000010));
    assert(bridge.convert(3000) == -1); // discontinuity / excessive drift
    bridge.include({});
    assert(!bridge.valid && bridge.convert(3000) == -1);

    // Both ring orientations: allow the next job while a result awaits XR,
    // then backpressure a third even after the synthetic frame was selected.
    for (int slot = 0; slot < 2; ++slot) {
        BufferedResultState result;
        assert(result.permitsGeneration(slot));
        result.publish(slot);
        assert(result.permitsGeneration(slot));
        assert(!result.permitsGeneration(1 - slot));
        result.endpoint = true;
        assert(!result.permitsGeneration(1 - slot));
        result = {}; // endpoint consumed, with its read fence retained by owner
        assert(result.permitsGeneration(1 - slot));
        result.publish(1 - slot);
        assert(!result.endpoint);
        assert(result.permitsGeneration(1 - slot));
        assert(!result.permitsGeneration(slot));
    }
    assert((inputReadinessSplit(100, 120, 150) == std::array<int64_t, 2>{20, 30}));
    assert((inputReadinessSplit(100, 80, 150) == std::array<int64_t, 2>{0, 50}));
    assert((inputReadinessSplit(100, 80, 90) == std::array<int64_t, 2>{0, 0}));
    assert((inputReadinessSplit(100, 150, 150) == std::array<int64_t, 2>{50, 0}));
    assert(inputReadinessSplit(100, 0, 150)[0] == -1);
    assert(inputReadinessSplit(100, 151, 150)[0] == -1);

    assert(gpuTimestampDuration(100, 150, 64, 2.5) == 125);
    assert(gpuTimestampDuration(250, 5, 8, 2) == 22);
    assert(gpuTimestampDuration(UINT64_MAX - 5, 4, 64, 1) == 10);
    assert(gpuTimestampDuration(0, 1, 0, 1) == -1);
    assert(gpuTimestampDuration(0, 1, 65, 1) == -1);
    assert(gpuTimestampDuration(0, 1, 64, 0) == -1);
    assert(gpuTimestampDuration(0, 1, 64, NAN) == -1);
    assert(gpuTimestampDuration(0, UINT64_MAX, 64, 1) == -1);
    assert(calibratedGpuTime(90,100,1000,64,2)==980);
    assert(calibratedGpuTime(110,100,1000,64,2)==1020);
    assert(calibratedGpuTime(250,5,1000,8,2)==978);
    assert(calibratedGpuTime(5,250,1000,8,2)==1022);
    assert(calibratedGpuTime(UINT64_MAX-5,4,1000,64,1)==990);
    assert(calibratedGpuTime(4,UINT64_MAX-5,1000,64,1)==1010);
    assert(calibratedGpuTime(128,0,1000,8,1)==-1); // ambiguous half-wrap
    assert(calibratedGpuTime(0,0,0,64,1)==-1);
    assert(calibratedGpuTime(0,0,1000,0,1)==-1);
    assert(calibratedGpuTime(0,0,1000,65,1)==-1);
    assert(calibratedGpuTime(0,0,1000,64,NAN)==-1);
    assert(calibratedGpuTime(0,0,1000,64,-1)==-1);
    assert(calibratedGpuTime(0,6000000000LL,10000000000LL,64,1)==-1); // stale
    assert(calibratedGpuTime(0,2000,1000,64,1)==-1); // invalid host result
    float id[] = {0, 0, 0, 1}, opposite[] = {0, 0, 0, -1};
    auto q = midpointRotation(id, opposite);
    near(q[3], 1);
    float yaw[] = {0, std::sin(.2f), 0, std::cos(.2f)};
    q = midpointRotation(id, yaw);
    near(q[1], std::sin(.1f));
    near(q[3], std::cos(.1f));
    auto m = targetToSourceRotation(yaw, yaw);
    for (int i = 0; i < 9; ++i)
        near(m[i], i % 4 == 0 ? 1 : 0);
    m = targetToSourceRotation(id, yaw);
    near(-m[6], -std::sin(.4f));
    near(-m[8], -std::cos(.4f));
    auto reverse = targetToSourceRotation(yaw, id);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) {
            float v = 0;
            for (int k = 0; k < 3; ++k)
                v += m[k * 3 + r] * reverse[c * 3 + k];
            near(v, r == c ? 1 : 0);
        }
    constexpr int64_t tick = 13888889, base = 1000000000;
    assert(interpolationInterval(base, base + tick * 2, tick));
    assert(!interpolationInterval(base, base + tick, tick));
    assert(!interpolationInterval(base, base - 1, tick));
    assert(!interpolationInterval(0, base, tick));
    assert(!interpolationInterval(base, base + 100000001, tick));
    float fov[] = {-.8f, .8f, .8f, -.8f};
    assert(compatibleFlowAnchor(id, fov, opposite, fov));
    float yaw4[] = {0, std::sin(.0349066f), 0, std::cos(.0349066f)};
    float yaw6[] = {0, std::sin(.0523599f), 0, std::cos(.0523599f)};
    assert(compatibleFlowAnchor(yaw4, fov, id, fov));
    assert(!compatibleFlowAnchor(yaw6, fov, id, fov));
    float changedFov[] = {-.81f, .8f, .8f, -.8f};
    assert(!compatibleFlowAnchor(id, changedFov, id, fov));
    float invalid[] = {0, 0, 0, 0};
    assert(!compatibleFlowAnchor(invalid, fov, id, fov));
    // Compare the packed UV homography with the capture shader's original
    // ray/FOV math, including asymmetric FOV and both vertical orientations.
    for (const auto* source : {id,yaw4,yaw6}) {
        const auto m=colorTransform(source,fov,id,changedFov).columns;
        const auto r=targetToSourceRotation(source,id);
        for(float u:{0.f,.25f,.75f,1.f}) for(float v:{0.f,.4f,1.f}) {
            const float x=std::tan(changedFov[0])+(std::tan(changedFov[1])-std::tan(changedFov[0]))*u;
            const float y=std::tan(changedFov[2])+(std::tan(changedFov[3])-std::tan(changedFov[2]))*v;
            const float z=-(r[2]*x+r[5]*y-r[8]);
            const float expectedU=((r[0]*x+r[3]*y-r[6])/z-std::tan(fov[0]))/(std::tan(fov[1])-std::tan(fov[0]));
            const float expectedV=((r[1]*x+r[4]*y-r[7])/z-std::tan(fov[2]))/(std::tan(fov[3])-std::tan(fov[2]));
            const float divisor=m[2]*u+m[6]*v+m[10];
            assert(std::abs((m[0]*u+m[4]*v+m[8])/divisor-expectedU)<1e-5);
            assert(std::abs((m[1]*u+m[5]*v+m[9])/divisor-expectedV)<1e-5);
        }
    }
    FlowHistory h;
    assert(!h.matches(base));
    for (int oldSlot = 0; oldSlot < 2; ++oldSlot) {
        h.reset();
        auto count = h.begin(false, oldSlot, base);
        assert(count == uint64_t(oldSlot + 1));
        for (int i = 1; i < 100; ++i) {
            assert(h.matches(base + (i - 1) * tick));
            assert(!h.matches(base + i * tick));
            assert(h.begin(true, (oldSlot + i) % 2, base + i * tick) == ++count);
            assert(count % 2 == uint64_t((oldSlot + i + 1) % 2));
        }
        h.reset();
        assert(!h.matches(base + 99 * tick));
    }

    assert(nominalDisplayPeriod(tick, tick) == tick);
    assert(nominalDisplayPeriod(0, tick) == tick);
    assert(nominalDisplayPeriod(11111111, tick) == 11111111);
    assert(!freshSynthetic(base, base));
    assert(freshSynthetic(base + 1, base));
    using O = PresentationChoice;
    // Ready results are selectable at the first XR tick, regardless of
    // processing cost or runtime prediction horizon. Endpoints retain dynamic
    // half-cadence spacing and never repeat their generated midpoint.
    for (int64_t lead : {0LL, 30000000LL, 65000000LL, 100000000LL}) {
        for (int64_t interval : {22000000LL, 33333333LL, 50000000LL, 75000000LL, 95000000LL}) {
            PresentationTimeline timeline;
            for (int n = 0; n < 120; ++n) {
                const auto arrival = base + n * interval;
                auto pair = timeline.begin(arrival, tick, n != 0, interval);
                const auto ready = arrival + (n % 10 == 0 ? 81000000 : n % 2 ? 54000000 : 66000000);
                const auto first = base + ((ready - base + tick - 1) / tick) * tick + lead;
                assert(duePresentation(pair, n != 0, first) == (n ? O::Synthetic : O::Real));
                PresentationTimeline::selected(pair, first);
                assert(pair.midpoint == first);
                assert(pair.real == first + (n ? std::max(tick, interval / 2) : 0));
                const auto savedReal = pair.real;
                PresentationTimeline::selected(pair, first + tick);
                assert(pair.real == savedReal); // later polls cannot shift deadlines
                if (n) {
                    assert(duePresentation(pair, true, first) == O::Synthetic);
                    assert(duePresentation(pair, false, pair.real - 1) == O::None);
                    assert(duePresentation(pair, false, pair.real) == O::Real);
                    assert(duePresentation(pair, true, pair.real + tick * 3) == O::Real);
                }
            }
        }
    }
    // A newer completed job must not evict the real endpoint of a midpoint
    // already selected, including just before and exactly at its deadline.
    for (int64_t period : {11111111LL, 13888889LL}) {
        PresentationTimeline timeline;
        auto pair = timeline.begin(base, period, true, 56000000);
        assert(mayReplacePresentation(pair, false, base, period));
        PresentationTimeline::selected(pair, base);
        assert(!mayReplacePresentation(pair, true, pair.real - 1, period));
        assert(!mayReplacePresentation(pair, true, pair.real, period));
        assert(!mayReplacePresentation(pair, true, pair.real + period, period));
        assert(duePresentation(pair, false, pair.real, period) == O::Real);
        assert(mayReplacePresentation(pair, true, pair.real + 500000000, period));
        // Without a selected midpoint, prefer a newer complete pair immediately.
        assert(mayReplacePresentation(pair, false, pair.real, period));
        // No wall-clock wait and no selecting two images for the same XR slot.
        assert(duePresentation(pair, false, base, period) == O::None);
        const auto justEarly = pair.real - period / 2;
        assert(duePresentation(pair, false, justEarly - 1, period) == O::None);
        assert(duePresentation(pair, false, justEarly, period) == O::Real);
        // An unshown midpoint must still get its slot, not be skipped early.
        assert(duePresentation(pair, true, justEarly, period) == O::Synthetic);
    }
    // Reproduce the device's approximate 56ms game / 28ms presentation cadence.
    // A 28ms ideal endpoint falls just AFTER the next 72Hz-derived slot. The
    // old policy repeated it, then discarded it for a newer midpoint.
    {
        PresentationTimeline timeline;
        auto pair = timeline.begin(base, tick, true, 56000000);
        PresentationTimeline::selected(pair, base);
        assert(duePresentation(pair, false, base + tick * 2) == O::None);
        assert(duePresentation(pair, false, base + tick * 2, tick) == O::Real);
        assert(!mayReplacePresentation(pair, true, base + tick * 2, tick));
        // If that opportunity was missed, the next slot still completes this
        // pair before a newer completed pair, rather than skipping its endpoint.
        assert(!mayReplacePresentation(pair, true, base + tick * 4, tick));
        assert(duePresentation(pair, false, base + tick * 4, tick) == O::Real);
    }
    // Exercise repeated pairs with varying game cadence and skipped XR slots.
    // The already-started pair retains priority even with fresher work ready
    // at every selection; long interruptions remain allowed to discard it.
    for (int64_t period : {11111111LL, 13888889LL, 16666667LL}) {
        for (int64_t interval : {33000000LL, 56000000LL, 95000000LL}) {
            for (int stride : {1, 2, 3}) {
                PresentationTimeline timeline;
                for (int n = 0; n < 40; ++n) {
                    const auto first = base + n * 200000000LL;
                    auto pair = timeline.begin(first, period, true, interval);
                    PresentationTimeline::selected(pair, first);
                    assert(duePresentation(pair, true, first, period) == O::Synthetic);
                    bool endpoint = false;
                    for (auto slot = first + period * stride;
                         slot < first + 150000000; slot += period * stride) {
                        assert(!mayReplacePresentation(pair, true, slot, period));
                        if (duePresentation(pair, false, slot, period) == O::Real) {
                            endpoint = true;
                            break;
                        }
                    }
                    assert(endpoint);
                }
            }
        }
    }
    // Consume only every third frame, while the transport sees every stereo
    // arrival. Backpressure must not turn 20 FPS into a 6.7 FPS cadence estimate.
    StereoArrivalCadence arrivals;
    PresentationTimeline skipped;
    for (uint64_t n = 1; n <= 180; ++n) {
        const auto at = base + n * 50000000;
        const auto interval = arrivals.observe(n, at);
        if (n % 3 == 0) {
            skipped.begin(at, tick, true, interval);
            assert(skipped.cadence() == 50000000);
        }
        // Repeated eye/frame metadata must not bias the estimate.
        assert(arrivals.observe(n, at + 1000000) == interval);
    }
    assert(arrivals.observe(181, base + 10000000000LL) == 0); // pause
    assert(arrivals.observe(182, base + 10050000000LL) == 50000000);
    assert(arrivals.observe(1, base + 10100000000LL) == 0); // source restart
    assert(arrivals.observe(2, base + 10000000000LL) == 0); // clock discontinuity
    PresentationTimeline dynamic;
    int64_t arrival = base;
    for (int n = 0; n < 100; ++n) {
        const int64_t interval = n < 40 ? 70000000 : 30000000;
        arrival += interval + (n % 3 - 1) * 1000000; // jitter and a large cadence change
        auto pair = dynamic.begin(arrival, tick, true);
        assert(pair.real == 0); // unfinished GPU work has no presentation reservation
        const auto ready = arrival + (n == 45 ? 150000000 : 8000000);
        PresentationTimeline::selected(pair, ready);
        assert(pair.midpoint == ready);
        assert(pair.real - ready < 100000000); // a slow job does not inflate later holds
    }
    assert(std::abs(dynamic.cadence() - 30000000) < 1000000);
    // Pause/clock discontinuity drops the old cadence and presentation timeline.
    arrival += 1000000000;
    auto resumed = dynamic.begin(arrival, tick, false);
    PresentationTimeline::selected(resumed, arrival);
    assert(resumed.real == arrival);
    assert(dynamic.cadence() == 0);
    dynamic.reset();
    auto seed = dynamic.begin(base, tick, false);
    PresentationTimeline::selected(seed, base);
    assert(seed.real == base);
    seed = dynamic.begin(base - 1, tick, false);
    assert(seed.real == 0 && seed.spacing == 0 && dynamic.cadence() == 0);
    std::cout << "VR pose, history ownership and completion-driven timeline tests passed\n";
}
