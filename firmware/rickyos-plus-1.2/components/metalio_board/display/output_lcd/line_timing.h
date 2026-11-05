#pragma once
#include <stdbool.h>
#include <stdint.h>

// Board timing planner, not an optical gray/dose model. Both clocks must have
// exactly the same line period: rounding RMT and LCD independently tears rows.
typedef struct {
    int cycles;
    int period_ticks;
    int high_ticks;
    int high_us10;
    int period_us;
} EpdLcdLineTiming;

static inline bool epd_lcd_plan_line_timing(
    uint32_t pclk, uint32_t rmt, int minimum_cycles, int default_cycles,
    int default_high_us10, int requested_period_us10, int requested_high_us10,
    EpdLcdLineTiming* out
) {
    if (!out || !pclk || !rmt || minimum_cycles <= 0 || default_cycles < minimum_cycles
        || requested_period_us10 < 0 || requested_high_us10 < 0) return false;
    const int period_us10 = requested_period_us10 ? requested_period_us10
        : (int)((int64_t)default_cycles * 10000000 / pclk);
    const int high_us10 = requested_high_us10 ? requested_high_us10 : default_high_us10;
    // No overclock or extended drive experiment. Preserve >=1us on BOTH CKV
    // levels and never widen the gate beyond the existing board setting.
    if (period_us10 < 20 || period_us10 > 1000 || high_us10 < 10
        || high_us10 > default_high_us10) return false;
    uint32_t a = pclk, b = rmt;
    while (b) { const uint32_t remainder = a % b; a = b; b = remainder; }
    const uint32_t quantum = pclk / a;
    int64_t cycles = requested_period_us10
        ? ((int64_t)period_us10 * pclk + 9999999) / 10000000 : default_cycles;
    cycles = (cycles + quantum - 1) / quantum * quantum;
    if (cycles < minimum_cycles || cycles > 65535) return false;
    const int64_t ticks = cycles * rmt / pclk;
    const int64_t high = ((int64_t)high_us10 * rmt + 9999999) / 10000000;
    const int64_t minimum = (rmt + 999999) / 1000000;
    if (high < minimum || ticks - high < minimum || high > 32767 || ticks - high > 32767)
        return false;
    *out = (EpdLcdLineTiming){
        .cycles = (int)cycles, .period_ticks = (int)ticks, .high_ticks = (int)high,
        .high_us10 = (int)(high * 10000000 / rmt),
        .period_us = (int)((cycles * 1000000 + pclk - 1) / pclk),
    };
    return true;
}
