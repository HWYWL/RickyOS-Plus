#include "portrait_i1.h"
#include "epd_font24.h"
#include <string.h>

bool metalio_portrait_i1_to_native(const uint8_t *src, size_t src_size, size_t stride,
                                   uint8_t *dst, size_t dst_size) {
    const size_t native_bytes = (size_t)EPD_LOGICAL_W * EPD_LOGICAL_H / 2;
    if (!src || !dst || stride < (EPD_LOGICAL_W + 7u) / 8u ||
        stride > SIZE_MAX / EPD_LOGICAL_H || src_size < stride * EPD_LOGICAL_H ||
        dst_size < native_bytes) return false;
    memset(dst, 0xff, native_bytes);
    for (int y = 8; y < EPD_LOGICAL_H - 8; ++y) {
        for (int x = 8; x < EPD_LOGICAL_W - 8; ++x) {
            if (src[(size_t)y * stride + (unsigned)x / 8] & (0x80u >> (x & 7))) continue;
            int px, py;
            epd_logical_to_physical(x, y, &px, &py);
            size_t offset = ((size_t)py * EPD_LOGICAL_H + px) / 2;
            dst[offset] &= (px & 1) ? 0x0f : 0xf0;
        }
    }
    return true;
}
