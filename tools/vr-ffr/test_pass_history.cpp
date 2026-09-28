#include "../../app/src/main/cpp/vrffr/pass_history.h"
#include <cassert>
int main() {
  using ffr::PassHistory;
  ffr::PassWork heavy{0, 30, 28, 100000}, cheap{0, 1, 0, 0};
  PassHistory h;
  for (unsigned frame = 1; frame <= 12; ++frame) {
    auto a = h.begin(frame, "same");
    assert(a.prediction.passes == frame - 1);
    if (frame > 1) assert(a.prediction.draws == heavy.draws);
    h.finish(a, heavy);
    auto b = h.begin(frame, "same");
    if (frame > 1) assert(b.prediction.draws == cheap.draws);
    h.finish(b, cheap);
  }
  // An unrelated insertion does not shift either identical-key occurrence.
  auto extra = h.begin(13, "extra"); h.finish(extra, cheap);
  auto a = h.begin(13, "same");
  assert(a.index == 0 && a.prediction.passes == 12);
  h.finish(a, heavy);
  auto b = h.begin(13, "same");
  assert(b.index == 1 && b.prediction.draws == cheap.draws); h.finish(b, cheap);
  // Missing suffixes and whole missing intervals preserve each track.
  a = h.begin(14, "same"); h.finish(a, heavy);
  a = h.begin(18, "same"); assert(a.prediction.passes == 14); h.finish(a, heavy);
  b = h.begin(18, "same"); assert(b.prediction.passes == 13); h.finish(b, cheap);
  // A single contradiction suppresses work estimates, but retains confidence.
  a = h.begin(19, "same"); h.finish(a, cheap);
  a = h.begin(20, "same");
  assert(a.prediction.passes == 16 && a.prediction.draws == cheap.draws);
  h.finish(a, heavy);
  for (unsigned f = 21; f <= 22; ++f) { a = h.begin(f, "same"); h.finish(a, heavy); }
  a = h.begin(23, "same");
  assert(a.prediction.passes >= 8 && a.prediction.draws == heavy.draws); h.finish(a, cheap);
  a = h.begin(24, "same"); h.finish(a, cheap);
  a = h.begin(25, "same"); h.finish(a, cheap);
  a = h.begin(26, "same"); assert(a.prediction.passes == 1); h.finish(a, cheap);
  // New attachment identities never inherit another track's expensive work.
  a = h.begin(27, "new-depth"); assert(a.prediction.passes == 0); h.finish(a, heavy);
  b = h.begin(27, "same"); assert(b.index == 0); h.finish(b, cheap);
  // Long absence expires only that track, not a recently used neighbor.
  for (unsigned f = 28; f <= 160; ++f) { a = h.begin(f, "new-depth"); h.finish(a, heavy); }
  b = h.begin(160, "same"); assert(b.prediction.passes == 0); h.finish(b, cheap);
  a = h.begin(161, "new-depth"); assert(a.prediction.passes > 120); h.finish(a, heavy);
  // A pending old recording does not wipe unrelated identities. Its late
  // completion cannot overwrite the replacement recording of the same track.
  a = h.begin(162, "new-depth");
  b = h.begin(163, "new-depth"); assert(b.prediction.passes == 0);
  auto before = h.slots[b.index].observations;
  h.finish(a, cheap); assert(h.slots[b.index].observations == before);
  h.finish(b, heavy); assert(h.slots[b.index].observations == before + 1);
  // A late finish is accepted if its track has not been reused, despite a new
  // projection interval starting for another writer.
  a = h.begin(164, "new-depth");
  b = h.begin(165, "same"); h.finish(b, cheap);
  h.finish(a, heavy); assert(!h.slots[a.index].pending);
  // Repeated scene iterations within an interval learn independent occurrences.
  PassHistory repeated;
  for (unsigned f = 1; f <= 12; ++f) {
    for (unsigned i = 0; i < 2; ++i) {
      auto t = repeated.begin(f, "scene");
      if (f > 8) assert(t.prediction.passes >= 8);
      repeated.finish(t, heavy);
    }
  }
  a = repeated.begin(13, "scene"); repeated.finish(a, heavy); // second absent
  a = repeated.begin(14, "scene"); assert(a.prediction.passes == 13); repeated.finish(a, heavy);
  b = repeated.begin(14, "scene"); assert(b.prediction.passes == 12); repeated.finish(b, heavy);
  // Bounded allocation; reclaiming old slots does not accept stale tickets.
  PassHistory bounded;
  assert(bounded.begin(0, "x").index == 64);
  for (unsigned i = 0; i < 100; ++i) {
    auto t = bounded.begin(1, "x"); bounded.finish(t, heavy);
  }
  assert(bounded.slots.size() == 64);
  assert(bounded.begin(2, "new").index == 64);
  a = bounded.begin(200, "new"); assert(a.index < 64 && a.prediction.passes == 0);
  bounded.finish(a, cheap);
}
