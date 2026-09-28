#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ffr {
struct Rect {
  uint32_t x = 0, y = 0, w = 0, h = 0;
};
struct Evidence {
  bool eligible = false, depth = false, connected = false, matchesEye = false;
  uint32_t observations = 0, draws = 0, indexed = 0;
  // Engine identity is a weak prior, never sufficient to select a target.
  bool knownEngine = false;
  uint32_t indexedVertices = 0;
  bool sampledConnection = false;
};
inline bool select(const Evidence &e, int mode) {
  if (!mode || !e.eligible || !e.depth || e.observations < 3) return false;
  // Inferred shader-input ancestry is restricted to Aggressive and substantial
  // geometry. Engine identity, image names and particular resolutions are irrelevant.
  if (mode == 2 && e.sampledConnection && e.observations >= 8 &&
      e.draws >= 2 && e.indexed >= 2 && e.indexedVertices >= 4096) return true;
  // Batching can reduce a substantial scene to a handful of draw calls. Only
  // relax the call threshold for proven eye ancestry and sustained, known work;
  // indirect-buffer contents are unknown and never receive this vertex credit.
  const bool batchedScene = e.connected && e.observations >= 8 &&
      e.indexed >= 2 && e.indexedVertices >= 768;
  if (!batchedScene && (e.draws < 8 || e.indexed < 4)) return false;
  if (e.connected)
    return true;
  if (mode != 2 || !e.matchesEye || e.observations < 8)
    return false;
  return e.draws >= (e.knownEngine ? 16u : 24u) && e.indexed >= 8;
}
// Pixel coordinates are local to an attachment; separate rectangles avoid
// placing a single high-quality region on the seam of packed stereo images.
inline uint8_t density(float x, float y, const Rect *eyes, unsigned count) {
  float r = 1000.f, outer = 1000.f;
  for (unsigned i = 0; i < count; i++) {
    auto e = eyes[i];
    if (!e.w || !e.h || x < e.x || y < e.y || x >= e.x + e.w || y >= e.y + e.h)
      continue;
    float dx = (x - e.x - e.w * .5f) / (e.w * .5f),
          dy = (y - e.y - e.h * .5f) / (e.h * .5f);
    r = std::min(r, std::sqrt(dx * dx + dy * dy));
    // Keep the full-rate ellipse unchanged, but bring 4x4 shading inward
    // horizontally: 80% of the half-width versus 105% of the half-height.
    outer = std::min(outer, std::sqrt((dx / .8f) * (dx / .8f) +
                                     (dy / 1.05f) * (dy / 1.05f)));
  }
  if (r < .48f)
    return 255;
  if (outer < 1.f)
    return 128;
  return 64;
}
} // namespace ffr
