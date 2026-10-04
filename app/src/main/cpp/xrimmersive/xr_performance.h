#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>

namespace xrimmersive {
inline int64_t performanceNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// One immersive session per process. Producers only increment atomics; bitmap
// generation and one-second aggregation run on a separate Android worker.
class VrPerformanceMetrics {
public:
    enum Stage { Presentation, Reserved, Generation };
    std::atomic<bool> visible{false};
    std::atomic<float> refresh{0};
    std::atomic<unsigned> used{0}; // FXAA=1, SGSR active=2, FG=4, fused FXAA=8, SGSR selected=16
    void reset() {
        visible=false; refresh=0; used=0; presents=0; games=0; selectionCounts=0; lastGame=0; inputSize=0; outputSize=0;
        std::lock_guard<std::mutex> lock(mutex);
        start=0; sequence=0; data={}; clearTimings(); lastStage={}; sampleMode={};
    }
    void effects(bool fxaa, bool sgsr, bool fused=false, bool sgsrSelected=false) {
        unsigned old=used.load();
        const unsigned bits=(fxaa?1u:0u)|(sgsr?2u:0u)|(fused?8u:0u)|(sgsrSelected?16u:0u);
        while(!used.compare_exchange_weak(old,(old&~27u)|bits)) {}
    }
    void resolution(uint32_t iw,uint32_t ih,uint32_t ow,uint32_t oh) {
        inputSize=(uint64_t(iw)<<32)|ih; outputSize=(uint64_t(ow)<<32)|oh;
    }
    void present() { if(visible.load(std::memory_order_relaxed)) ++presents; }
    void selected(bool repeated) {
        if(visible.load(std::memory_order_relaxed)) { selectionCounts.fetch_add(1ull+(repeated ? (1ull<<32) : 0)); }
    }
    void game(uint64_t id) {
        if(id && lastGame.exchange(id)!=id && visible.load(std::memory_order_relaxed)) ++games;
    }
    void stage(Stage stage, int64_t ns, int64_t now=performanceNow(), int expectedMode=-1) {
        if(ns<0 || !visible.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(mutex);
        const unsigned mode=used.load()&3u;
        if(stage==Presentation && expectedMode>=0 && mode!=unsigned(expectedMode)) return;
        if(stage==Presentation && sampleMode[stage]!=mode) { sums[stage]=0; counts[stage]=0; }
        sums[stage]+=ns; ++counts[stage]; lastStage[stage]=now; sampleMode[stage]=mode;
    }
    void setVisible(bool value, int64_t now=performanceNow()) {
        std::lock_guard<std::mutex> lock(mutex);
        visible=value; presents=0; games=0; selectionCounts=0; start=now; sequence=0; clearTimings(); lastStage={}; sampleMode={};
        data.fill(std::numeric_limits<double>::quiet_NaN());
    }
    // visible, sequence, Hz, present FPS, game FPS, presentation/reserved/FG ms, used mask, source W/H, output W/H (left eye), repeated selection %.
    std::array<double,14> snapshot(int64_t now=performanceNow()) {
        std::lock_guard<std::mutex> lock(mutex);
        if(visible && now-start>=1000000000LL) {
            const auto countsSnapshot=selectionCounts.exchange(0);
            const auto selectedCount=countsSnapshot&0xffffffffu, repeatedCount=countsSnapshot>>32;
            data[13]=selectedCount ? 100.0*std::min(repeatedCount,selectedCount)/selectedCount
                                   : std::numeric_limits<double>::quiet_NaN();
            const double seconds=(now-start)/1e9;
            data[3]=presents.exchange(0)/seconds; data[4]=games.exchange(0)/seconds;
            for(unsigned i=0;i<3;++i) {
                if(counts[i]) data[5+i]=sums[i]/double(counts[i])/1e6;
                // Sparse queries stay readable between samples, but never forever.
                if(!lastStage[i] || now-lastStage[i]>10000000000LL ||
                   (i==Presentation && sampleMode[i]!=(used.load()&3u)))
                    data[5+i]=std::numeric_limits<double>::quiet_NaN();
            }
            clearTimings(); start=now; ++sequence;
        }
        data[0]=visible ? 1:0; data[1]=sequence; data[2]=refresh; data[8]=used;
        const auto input=inputSize.load(), output=outputSize.load();
        data[9]=input>>32; data[10]=input&0xffffffffu;
        data[11]=output>>32; data[12]=output&0xffffffffu;
        return data;
    }
private:
    std::atomic<uint64_t> selectionCounts{0};
    std::atomic<uint64_t> presents{0}, games{0}, lastGame{0};
    std::atomic<uint64_t> inputSize{0}, outputSize{0};
    std::array<int64_t,3> lastStage{};
    std::array<unsigned,3> sampleMode{};
    std::mutex mutex;
    int64_t start=0;
    uint64_t sequence=0;
    std::array<double,14> data{};
    std::array<int64_t,3> sums{};
    std::array<uint64_t,3> counts{};
    void clearTimings() { sums={}; counts={}; }
};
inline VrPerformanceMetrics vrPerformance;

// Latch until BOTH sticks have been released for 150 ms. Suppress the existing
// double-click gesture for the entire chord, including staggered releases.
class VrPerformanceChord {
public:
    bool update(bool left, bool right, int64_t now) {
        if(latched) {
            if(left || right) releasedAt=0;
            else if(!releasedAt) releasedAt=now;
            else if(now-releasedAt>=150000000LL) latched=false;
            return false;
        }
        if(left && right) { latched=true; releasedAt=0; return true; }
        return false;
    }
    bool consumed() const { return latched; }
private:
    bool latched=false;
    int64_t releasedAt=0;
};
}
