#include "../../app/src/main/cpp/xrimmersive/xr_performance.h"
#include <cassert>
#include <cmath>
#include <thread>
using namespace xrimmersive;
int main() {
    VrPerformanceMetrics m;
    m.reset(); m.refresh=72; m.used=7;
    m.setVisible(true,1000000000);
    for(unsigned i=0;i<72;++i) m.present();
    for(unsigned i=1;i<=36;++i) {m.game(i);m.game(i);}
    m.stage(VrPerformanceMetrics::Presentation,1500000,1500000000);
    m.stage(VrPerformanceMetrics::Generation,8000000,1500000000);
    m.stage(VrPerformanceMetrics::Generation,10000000,1500000000);
    for(unsigned i=0;i<72;++i) m.selected(i%2==0);
    auto before=m.snapshot(1999999999);
    assert(before[1]==0 && std::isnan(before[3]));
    auto s=m.snapshot(2000000000);
    assert(s[1]==1 && s[2]==72 && s[3]==72 && s[4]==36);
    assert(s[13]==50);
    assert(s[5]==1.5 && std::isnan(s[6]) && s[7]==9 && s[8]==7);
    s=m.snapshot(3000000000);
    assert(s[3]==0 && s[4]==0 && s[5]==1.5 && s[7]==9); // Retain sparse GPU samples.
    assert(std::isnan(s[13]));
    m.effects(true,false,true,true);
    m.resolution(2014,2216,1964,2160);
    s=m.snapshot(4000000000);
    assert(std::isnan(s[5]) && s[8]==29 && s[9]==2014 && s[12]==2160);
    m.stage(VrPerformanceMetrics::Presentation,999000000,4500000000,0); // Query from a different filter combination rejected.
    m.stage(VrPerformanceMetrics::Presentation,18000000,4500000000,1);
    assert(m.snapshot(5000000000)[5]==18);
    s=m.snapshot(16000000000);
    assert(std::isnan(s[5]) && std::isnan(s[7]));
    m.setVisible(false,17000000000);
    m.present(); m.game(200); m.stage(VrPerformanceMetrics::Presentation,500000000);
    m.setVisible(true,18000000000);
    s=m.snapshot(19000000000);
    assert(s[3]==0 && s[4]==0 && std::isnan(s[5]));
    // Two hot producers may run independently of the snapshot/UI worker.
    std::thread a([&] { for(int i=0;i<10000;++i)m.present(); });
    std::thread b([&] { for(int i=0;i<10000;++i)m.present(); });
    a.join();b.join();
    assert(m.snapshot(20000000000)[3]==20000);

    m.effects(false,false);
    m.stage(VrPerformanceMetrics::Presentation,2000000,20500000000,0);
    assert(m.snapshot(21000000000)[5]==2); // No AA/SGSR still reports PRESENT.

    VrPerformanceChord chord;
    assert(!chord.update(true,false,1000000000));
    assert(chord.update(true,true,1020000000));
    assert(!chord.update(true,true,1030000000));
    assert(!chord.update(false,true,1040000000));
    assert(!chord.update(true,true,1050000000)); // contact bounce
    assert(!chord.update(false,false,1060000000));
    assert(!chord.update(false,false,1150000000));
    assert(chord.consumed());
    assert(!chord.update(false,false,1220000000));
    assert(!chord.consumed());
    assert(chord.update(true,true,1230000000));
}
