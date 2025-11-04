#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Downscale an RGB565 image to half width and half height using nearest-neighbor.
// dst must be at least (src_w/2 * src_h/2 * 2) bytes.
void rgb565_downscale_half(const uint16_t *src,
                           uint16_t src_w,
                           uint16_t src_h,
                           uint16_t *dst);

#ifdef __cplusplus
}
#endif
