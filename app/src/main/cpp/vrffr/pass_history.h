#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ffr {
struct PassWork {
  uint32_t passes = 0, draws = 0, indexed = 0, indexedVertices = 0;
};
// Projection submissions delimit occurrence counting, not identity/lifetime.
// Unrelated insertions and temporarily absent writers do not invalidate a scene.
class PassHistory {
public:
  struct Slot {
    std::string key;
    std::array<PassWork, 3> recent{};
    uint32_t observations = 0;
    bool pending = false;
    uint32_t occurrence = 0, contradictions = 0;
    bool geometry = false;
    uint64_t lastSeen = 0, token = 0;
  };
  struct Ticket {
    uint64_t frame = 0, generation = 0;
    size_t index = 64;
    PassWork prediction{};
  };
  std::vector<Slot> slots;
  uint64_t frame = 0, generation = 1;
  std::unordered_map<std::string, uint32_t> occurrences;

  Ticket begin(uint64_t nextFrame, const std::string &key) {
    if (!nextFrame) return {};
    if (frame != nextFrame) {
      occurrences.clear();
      frame = nextFrame;
    }
    // Bound even a frame containing thousands of unrelated attachment scopes.
    if (occurrences.size() >= 64 && !occurrences.count(key)) return {};
    auto &next = occurrences[key];
    if (next >= 64) return {};
    const uint32_t occurrence = next++;
    size_t index = 0;
    for (; index < slots.size(); ++index)
      if (slots[index].key == key && slots[index].occurrence == occurrence) break;
    if (index == slots.size()) {
      if (slots.size() < 64) slots.push_back({});
      else {
        // Reclaim only expired, completed tracks; never evict a live scene to
        // accommodate transient passes. Per-track tokens reject stale tickets.
        for (index = 0; index < slots.size(); ++index)
          if (!slots[index].pending && expired(slots[index], frame)) break;
        if (index == slots.size()) return {};
      }
      slots[index] = Slot{key};
      slots[index].occurrence = occurrence;
    }
    auto &slot = slots[index];
    if (expired(slot, frame)) {
      slot.observations = slot.contradictions = 0;
      slot.pending = false;
    }
    Ticket t{frame, ++generation, index, {}};
    // A still-recording occurrence is ambiguous. Withhold this prediction,
    // supersede its ticket, but preserve the learned history of other writers.
    if (!slot.pending && slot.observations) {
      t.prediction = slot.recent[0];
      for (unsigned i = 1; i < std::min(slot.observations, 3u); ++i) {
        t.prediction.draws = std::min(t.prediction.draws, slot.recent[i].draws);
        t.prediction.indexed = std::min(t.prediction.indexed, slot.recent[i].indexed);
        t.prediction.indexedVertices = std::min(t.prediction.indexedVertices, slot.recent[i].indexedVertices);
      }
      t.prediction.passes = slot.observations;
    }
    slot.pending = true;
    slot.lastSeen = frame;
    slot.token = t.generation;
    return t;
  }
  void finish(const Ticket &t, PassWork actual) {
    if (t.index >= slots.size()) return;
    auto &slot = slots[t.index];
    if (!slot.pending || slot.token != t.generation) return;
    slot.pending = false;
    const bool geometry = actual.indexed >= 2 && actual.indexedVertices >= 4096;
    if (!slot.observations) slot.geometry = geometry;
    // One empty/cheap iteration affects the conservative recent-work estimate,
    // but does not erase warm-up. Repeated contradictions relearn this track.
    slot.contradictions = geometry != slot.geometry ? slot.contradictions + 1 : 0;
    if (slot.contradictions >= 3) {
      slot.observations = slot.contradictions = 0;
      slot.geometry = geometry;
    }
    slot.recent[slot.observations % 3] = actual;
    if (slot.observations >= 999) slot.observations = 996;
    ++slot.observations;
  }
private:
  static bool expired(const Slot &slot, uint64_t now) {
    return slot.lastSeen && (now < slot.lastSeen || now - slot.lastSeen > 120);
  }
};
} // namespace ffr
