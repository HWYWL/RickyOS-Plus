// SPDX-License-Identifier: LGPL-3.0-or-later
// RickyOS experimental native page-turn scheduling; no pixel translation.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t style;       // 0 off, 1 skim, 2 shallow arc, 3 ripple
    uint8_t orientation; // GfxRenderer: portrait, landscape CW, portrait inverted, landscape CCW
    bool from_right;     // logical screen direction, independent of the panel mounting
    bool has_origin;
    uint16_t origin_x, origin_y; // logical normalized 0..1023; not mirrored again for RTL
} RickyNativeTurn;

typedef struct {
    int x, y, width, height; // physical animated content, excludes chrome
    uint8_t spread; // 0 = original reader; UI uses completion-locked short reveal
} RickyNativeTurnRegion;

enum { RICKY_UI_TURN_SPREAD = 8 };

// Single display-owner task, consumed by the next draw. Clear explicitly after a
// no-difference update (which does not call epd_draw_base).
void epd_set_native_turn(uint8_t style, uint8_t orientation, bool from_right);
// Reader-only completion alignment; independent of spatial motion and AA rendering.
void epd_set_native_reader_text_finish(bool enabled);
void epd_set_native_ripple_origin(uint16_t x, uint16_t y);
void epd_set_native_ui_turn(uint8_t orientation, bool from_right, int x, int y, int width, int height);

// Integer sqrt runs only during preparation of the existing 16x16 delay grid,
// never in the ISR or line feeder. No floating-point or extra frame allocation.
static inline uint32_t ricky_turn_isqrt(uint64_t value) {
    uint64_t result = 0, bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else result >>= 1;
        bit >>= 2;
    }
    return (uint32_t)result;
}

typedef struct { int x, y; uint32_t radius; } RickyRippleGeometry;
static inline RickyRippleGeometry ricky_ripple_geometry(RickyNativeTurn turn, int w, int h) {
    RickyRippleGeometry g = {0, 0, 0};
    if (w < 2 || h < 2 || turn.orientation > 3) return g;
    const bool rotated = turn.orientation == 0 || turn.orientation == 2;
    const int lw = rotated ? h : w, lh = rotated ? w : h;
    const bool origin = turn.has_origin && turn.origin_x <= 1023 && turn.origin_y <= 1023;
    const int cx = origin ? (int)((int64_t)turn.origin_x * (lw - 1) / 1023) : (turn.from_right ? lw - 1 : 0);
    const int cy = origin ? (int)((int64_t)turn.origin_y * (lh - 1) / 1023) : (lh - 1) / 2;
    // Rotate the origin once; Euclidean distance is invariant under rotation.
    switch (turn.orientation) {
        case 0:g.x=cy;g.y=h-1-cx;break;
        case 1:g.x=w-1-cx;g.y=h-1-cy;break;
        case 2:g.x=w-1-cy;g.y=cx;break;
        default:g.x=cx;g.y=cy;break;
    }
    const int64_t farX = g.x > w - 1 - g.x ? g.x : w - 1 - g.x;
    const int64_t farY = g.y > h - 1 - g.y ? g.y : h - 1 - g.y;
    g.radius = ricky_turn_isqrt((uint64_t)(farX * farX + farY * farY));
    return g;
}
static inline int ricky_ripple_axis(RickyRippleGeometry g, int x, int y) {
    const int64_t dx = (int64_t)x - g.x, dy = (int64_t)y - g.y;
    const uint32_t distance = ricky_turn_isqrt((uint64_t)(dx * dx + dy * dy));
    return g.radius ? (int)((uint64_t)distance * 1024 / g.radius) : 0;
}

// Pure, bounded geometry shared by both boards and host acceptance tests.
static inline int ricky_turn_axis(RickyNativeTurn turn, int x, int y, int w, int h) {
    if (turn.style == 3) return ricky_ripple_axis(ricky_ripple_geometry(turn,w,h),x,y);
    int lx, ly, lw, lh;
    switch (turn.orientation) {
        case 0: lx = h - 1 - y; ly = x; lw = h; lh = w; break;
        case 1: lx = w - 1 - x; ly = h - 1 - y; lw = w; lh = h; break;
        case 2: lx = y; ly = w - 1 - x; lw = h; lh = w; break;
        default: lx = x; ly = y; lw = w; lh = h; break;
    }
    if (lw < 2 || lh < 2 || turn.style < 1 || turn.style > 3) return 0;
    int axis = (turn.from_right ? lw - 1 - lx : lx) * 1024 / (lw - 1);
    if (turn.style == 2) {
        const int v = ly * 2048 / (lh - 1) - 1024;
        const int bow = 1024 - v * v / 1024;
        axis = (axis * 100 + bow * 16) / 116;
    }
    return axis;
}

static inline uint8_t ricky_ui_turn_delay(RickyNativeTurn turn, int x, int y, int w, int h, int spread) {
    // Linear reveal: nine evenly spaced launch positions instead of quantizing
    // an already quantized 0..7 reader curve into only five broad bands.
    return (uint8_t)((ricky_turn_axis(turn, x, y, w, h) * spread + 512) / 1024);
}

#ifndef RICKYOS_PLUS_READER_FINE_RIPPLE
#define RICKYOS_PLUS_READER_FINE_RIPPLE 0
#endif
#if RICKYOS_PLUS_READER_FINE_RIPPLE != 0 && RICKYOS_PLUS_READER_FINE_RIPPLE != 1
#error "Fine reader ripple must be 0 or 1"
#endif

#ifndef RICKYOS_PLUS_READER_RIPPLE_SPREAD
#define RICKYOS_PLUS_READER_RIPPLE_SPREAD 23
#endif
#if RICKYOS_PLUS_READER_RIPPLE_SPREAD < 4 || RICKYOS_PLUS_READER_RIPPLE_SPREAD > 23
#error "Reader ripple spread must be 4..23"
#endif
#ifndef RICKYOS_PLUS_READER_SWEEP_SPREAD
#define RICKYOS_PLUS_READER_SWEEP_SPREAD 7
#endif
#if RICKYOS_PLUS_READER_SWEEP_SPREAD < 2 || RICKYOS_PLUS_READER_SWEEP_SPREAD > 7
#error "Reader sweep spread must be 2..7"
#endif

// Keep default/Pico/UI unchanged. Only opt-in S31 ripple uses10..24 launches.
// Shorter spread removes stagger-only scans, never the per-pixel waveform.
static inline int ricky_reader_turn_spread(RickyNativeTurn turn) {
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_FINE_RIPPLE
    if (turn.style == 3) return RICKYOS_PLUS_READER_RIPPLE_SPREAD;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    if (turn.style == 1 || turn.style == 2) return RICKYOS_PLUS_READER_SWEEP_SPREAD;
#endif
    return turn.style ? 7 : 0;
}
static inline uint8_t ricky_ripple_delay(int axis, int spread) {
    const int64_t t = axis < 0 ? 0 : axis > 1024 ? 1024 : axis;
    return (uint8_t)((spread * t * t * (3072 - 2 * t) + (1LL << 29)) >> 30);
}

static inline uint8_t ricky_turn_ease(int axis) {
    // Smoothstep launch schedule, rounded to eight wavefront positions.
    const int64_t t = axis;
    return (uint8_t)((7 * t * t * (3072 - 2 * t) + (1LL << 29)) >> 30);
}
static inline uint8_t ricky_turn_delay(RickyNativeTurn turn, int x, int y, int w, int h) {
    return ricky_turn_ease(ricky_turn_axis(turn, x, y, w, h));
}

static inline int ricky_turn_phase(int tick, int delay, int phases) {
    const int phase = tick - delay;
    return phase >= 0 && phase < phases ? phase : -1;
}
