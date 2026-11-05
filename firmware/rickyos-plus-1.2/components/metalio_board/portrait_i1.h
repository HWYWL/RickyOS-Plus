#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Source: portrait 684x1216, MSB-first rows, 1=white, stride >=86 bytes.
// Destination: native 1216x684, low nibble is even x, 4bpp, 0xf=white.
// Four 8px borders remain white. No allocation and no hardware access.
bool metalio_portrait_i1_to_native(const uint8_t *src, size_t src_size, size_t stride,
                                   uint8_t *dst, size_t dst_size);
#ifdef __cplusplus
}
#endif
