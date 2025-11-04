#include "rgb565_downscale.h"

void rgb565_downscale_half(const uint16_t *src,
                           uint16_t src_w,
                           uint16_t src_h,
                           uint16_t *dst)
{
    if (!src || !dst || src_w < 2 || src_h < 2) return;
    const uint16_t dst_w = src_w / 2;
    const uint16_t dst_h = src_h / 2;
    for (uint16_t y = 0; y < dst_h; ++y) {
        const uint16_t *row0 = src + (y * 2) * src_w;
        uint16_t *drow = dst + y * dst_w;
        for (uint16_t x = 0; x < dst_w; ++x) {
            // Nearest neighbor: pick top-left of 2x2 block
            drow[x] = row0[x * 2];
        }
    }
}
