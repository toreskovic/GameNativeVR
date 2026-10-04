#pragma once
#include <stdint.h>
/* Packing v1: even-sized outer quarters shrink by 2; the center is an
 * integer texel translation, including odd resolutions (no extra blur). */
static inline uint32_t gn_packed_extent(uint32_t logical) {
    return logical - (logical / 8u) * 2u;
}

/* Normalized low cuts and logical-to-physical scales, computed once on CPU. */
static inline void gn_packing_parameters(uint32_t w, uint32_t h, float out[4]) {
    if (!w) w = 1;
    if (!h) h = 1;
    out[0] = (float)((w / 8u) * 2u) / w;
    out[1] = (float)((h / 8u) * 2u) / h;
    out[2] = (float)w / gn_packed_extent(w);
    out[3] = (float)h / gn_packed_extent(h);
}
