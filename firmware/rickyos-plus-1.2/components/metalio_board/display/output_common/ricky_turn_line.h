// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "../ricky_native_turn.h"

// Immutable for the duration of both producers. Inline this inexpensive
// selector so a 16px run never calls a Flash-resident scheduling helper.
static inline __attribute__((always_inline)) int ricky_turn_window_spread(const RenderContext_t* ctx) {
    if (ctx->turn_region.spread) return ctx->turn_region.spread;
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_FINE_RIPPLE
    if (ctx->cycle_frames - ctx->turn_phases == RICKYOS_PLUS_READER_RIPPLE_SPREAD)
        return RICKYOS_PLUS_READER_RIPPLE_SPREAD;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    if (ctx->cycle_frames - ctx->turn_phases == RICKYOS_PLUS_READER_SWEEP_SPREAD)
        return RICKYOS_PLUS_READER_SWEEP_SPREAD;
#endif
    return 7;
}

static inline void ricky_turn_prepare_window(RenderContext_t* ctx) {
    if (!ctx->turn_window) return;
    // Both feeders finished the previous production; the ISR may still be
    // draining their queue tail. Fill this ring before notifying either next
    // producer. Only internal queued rows, never this ring, are read by the ISR.
    const int spread = ricky_turn_window_spread(ctx);
    const int slots = spread + 1;
    int lo = ctx->turn_producer_tick > spread
        ? ctx->turn_producer_tick - spread : 0;
    const int hi = ctx->turn_producer_tick < ctx->turn_phases ? ctx->turn_producer_tick : ctx->turn_phases - 1;
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_FINE_RIPPLE
    if (!ctx->turn_region.spread && spread == RICKYOS_PLUS_READER_RIPPLE_SPREAD) {
        // Sequential ticks retain every still-live ring slot. Only one phase
        // enters the window; after the final source phase no copy is needed.
        // A new transaction, repeated tick, jump or rewind seeds the complete
        // needed range instead. Both workers have joined before any write.
        if (ctx->turn_window_tick >= 0 &&
            ctx->turn_producer_tick == ctx->turn_window_tick + 1)
            lo = ctx->turn_producer_tick;
        ctx->turn_window_tick = ctx->turn_producer_tick;
    }
#endif
    for (int phase = lo; phase <= hi; ++phase) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        uint8_t* dest = ctx->turn_window + (phase % slots) * 256;
        const uint8_t* source = ctx->turn_luts + phase * 1024;
        for (int pair = 0; pair < 256; ++pair) dest[pair] = source[pair * 4];
#else
        memcpy(ctx->turn_window + (phase % slots) * 1024, ctx->turn_luts + phase * 1024, 1024);
#endif
    }
}

static inline __attribute__((always_inline)) const uint8_t* ricky_turn_lut(RenderContext_t* ctx, int phase) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    const int stride = 256;
#else
    const int stride = 1024;
#endif
    return ctx->turn_window ? ctx->turn_window + (phase % (ricky_turn_window_spread(ctx) + 1)) * stride
                            : ctx->turn_luts + phase * 1024;
}

// The same waveform actions as the existing padded SIMD LUT. Used only
// for the few chrome words at content edges; no row scratch or second DMA pass.
static inline uint8_t IRAM_ATTR ricky_turn_word(RenderContext_t* ctx, const uint32_t* input, const uint8_t* lut) {
    if (!lut) return 0;
    const uint8_t* pixels = (const uint8_t*)input;
    uint8_t result = 0;
    for (int lane = 0; lane < 4; ++lane) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        result |= lut[pixels[lane] * (ctx->turn_window ? 1 : 4)] << (6 - lane * 2);
#else
        (void)ctx;
        result |= lut[pixels[lane] * 4] << (lane * 2);
#endif
    }
    return result;
}

// S31's C path needs byte actions, not the Xtensa SIMD LUT padding. Preserve the
// original four-word unroll and MSB-first pixel order without PSRAM hot lookups.
static inline void IRAM_ATTR ricky_turn_lookup(RenderContext_t* ctx, const uint32_t* input,
                                     uint8_t* output, const uint8_t* lut, unsigned width) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    if (ctx->turn_window) {
#define RICKY_ACTION4(in) ((uint8_t)(lut[(in) & 255u] << 6 | lut[((in) >> 8) & 255u] << 4 | \
                                    lut[((in) >> 16) & 255u] << 2 | lut[(in) >> 24]))
        unsigned j = 0, n = width / 4;
        for (; j + 4 <= n; j += 4) {
            const uint32_t a = input[j], b = input[j + 1], c = input[j + 2], d = input[j + 3];
            output[j] = RICKY_ACTION4(a); output[j + 1] = RICKY_ACTION4(b);
            output[j + 2] = RICKY_ACTION4(c); output[j + 3] = RICKY_ACTION4(d);
        }
        for (; j < n; ++j) output[j] = RICKY_ACTION4(input[j]);
#undef RICKY_ACTION4
        return;
    }
#endif
    ctx->lut_lookup_func(input, output, lut, width);
}

static inline uint8_t ricky_turn_content_mask(int x, int left, int right) {
    uint8_t mask = 0;
    for (int lane = 0; lane < 4; ++lane) {
        if (x + lane < left || x + lane >= right) continue;
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        mask |= 3u << (6 - lane * 2);
#else
        mask |= 3u << (lane * 2);
#endif
    }
    return mask;
}

// Runs are aligned to 16 source pixels for the existing SIMD/C LUT converters.
// In portrait, skim is usually one whole-line run. Arc is at most eight runs.
static inline void ricky_turn_line(RenderContext_t* ctx, const uint32_t* input,
                                    uint8_t* output, int row) {
    const RickyNativeTurnRegion r = ctx->turn_region;
    const uint8_t* normal = ctx->turn_producer_tick < ctx->turn_phases
        ? ricky_turn_lut(ctx, ctx->turn_producer_tick) : NULL;
    if (r.spread && (row < r.y || row >= r.y + r.height)) {
        if (normal) ricky_turn_lookup(ctx, input, output, normal, ctx->display_width);
        else memset(output, 0, ctx->display_width / 4);
        return;
    }
    const uint8_t* delays = ctx->turn_delays + (row / 16) * ctx->turn_cols;
    for (int col = 0; col < ctx->turn_cols;) {
        const int start = col;
        const int delay = delays[col++];
        while (col < ctx->turn_cols && delays[col] == delay) ++col;
        const int x = start * 16;
        const int end = col * 16 < ctx->display_width ? col * 16 : ctx->display_width;
        const int phase = ricky_turn_phase(ctx->turn_producer_tick, delay, ctx->turn_phases);
        if (phase < 0)
            memset(output + x / 4, 0, (end - x) / 4); // HOLD before launch and after completion
        else
            ricky_turn_lookup(ctx, input + x / 4, output + x / 4,
                                 ricky_turn_lut(ctx, phase), end - x);
    }
    if (r.spread) {
        // Exact pixel boundaries: a 16px scheduling tile must never animate
        // a header/footer pixel just because its centre lies inside the content.
        const int right = r.x + r.width;
        const int firstFull = (r.x + 3) / 4;
        const int lastFull = right / 4;
        // Whole chrome spans must use the same vector converter as content.
        // The original scalar overwrite looked up hundreds of pixels in PSRAM
        // on EVERY row/phase and exceeded the LCD feeder's line deadline.
        const int leftAligned = r.x / 16 * 16;
        const int rightAligned = ((right + 15) / 16 * 16 < ctx->display_width)
            ? (right + 15) / 16 * 16 : ctx->display_width;
        if (leftAligned) {
            if (normal) ricky_turn_lookup(ctx, input, output, normal, leftAligned);
            else memset(output, 0, leftAligned / 4);
        }
        if (rightAligned < ctx->display_width) {
            if (normal) ricky_turn_lookup(ctx, input + rightAligned / 4, output + rightAligned / 4,
                                             normal, ctx->display_width - rightAligned);
            else memset(output + rightAligned / 4, 0, (ctx->display_width - rightAligned) / 4);
        }
        // At most eight output words (32 pixels) need scalar edge blending,
        // even on the full 1216px panel. No extra row buffer or DMA pass.
        for (int word = leftAligned / 4; word < firstFull; ++word) {
            const uint8_t mask = ricky_turn_content_mask(word * 4, r.x, right);
            output[word] = (output[word] & mask) | (ricky_turn_word(ctx, input + word, normal) & (uint8_t)~mask);
        }
        for (int word = lastFull > firstFull ? lastFull : firstFull; word < rightAligned / 4; ++word) {
            const uint8_t mask = ricky_turn_content_mask(word * 4, r.x, right);
            output[word] = (output[word] & mask) | (ricky_turn_word(ctx, input + word, normal) & (uint8_t)~mask);
        }
    }
}
