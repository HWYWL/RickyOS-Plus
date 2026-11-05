// SPDX-License-Identifier: LGPL-3.0-or-later
// Included by render.c; all scratch is owned until both feeders have joined.
#include "ricky_native_turn.h"
static RickyNativeTurn s_next_native_turn;
static RickyNativeTurnRegion s_next_native_region;
static bool s_next_reader_text_finish;
#ifndef RICKYOS_PLUS_READER_EARLY_ERASE
#define RICKYOS_PLUS_READER_EARLY_ERASE 0
#endif
#if RICKYOS_PLUS_READER_EARLY_ERASE != 0 && RICKYOS_PLUS_READER_EARLY_ERASE != 1
#error "Early reader erase must be 0 or 1"
#endif
#if RICKYOS_PLUS_READER_EARLY_ERASE && !RICKYOS_PLUS_READER_QUALITY
#error "Early reader erase requires the complete quality waveform"
#endif
#ifndef RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES
#define RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES 0
#endif
#if RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES < 0 || RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES > 8
#error "Reader erase overlap must be 0..8 phases"
#endif
#if RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES && (!RICKYOS_PLUS_READER_EARLY_ERASE || !RICKYOS_PLUS_READER_QUALITY)
#error "Reader erase overlap requires early erase and the complete quality waveform"
#endif
// Ripple keeps the departing text longer still: its rings spread the erase across the
// page, so the white between erase and write reads as a flash before the animation.
#ifndef RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES
#define RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES
#endif
#if RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES < 0 || RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES > 16
#error "Ripple erase overlap must be 0..16 phases"
#endif
#if RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES && (!RICKYOS_PLUS_READER_EARLY_ERASE || !RICKYOS_PLUS_READER_QUALITY)
#error "Ripple erase overlap requires early erase and the complete quality waveform"
#endif
#define RICKY_READER_ANY_OVERLAP (RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES || RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES)
#ifndef RICKYOS_PLUS_READER_EDGE_ERASE_WITH_CORE
#define RICKYOS_PLUS_READER_EDGE_ERASE_WITH_CORE 0
#endif
#if RICKYOS_PLUS_READER_EDGE_ERASE_WITH_CORE && (!RICKYOS_PLUS_READER_EARLY_ERASE || !RICKYOS_PLUS_READER_QUALITY)
#error "Edge erase alignment requires early erase and the complete quality waveform"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31)
// This renderer has one serialized context. Reserve its bounded hot working set
// in HP DRAM at link time, not in the fragmented reading heap. S31's C feeder
// needs only the low action byte of each padded 32-bit LUT entry: 9 * 256 B.
// No full-frame/static phase bank, and no ISR accesses this producer-only table.
// Direct byte actions deliberately trade 3840 B of bounded static storage
// for eliminating per-pixel packed decoding, which underruns on the real panel.
#if RICKYOS_PLUS_READER_FINE_RIPPLE
enum { RICKY_NATIVE_HOT_SLOTS = 24 };
#else
enum { RICKY_NATIVE_HOT_SLOTS = 9 };
#endif
static DRAM_ATTR uint8_t s_native_turn_window[RICKY_NATIVE_HOT_SLOTS * 256] __attribute__((aligned(16)));
#endif

void epd_set_native_turn(uint8_t style, uint8_t orientation, bool from_right) {
    s_next_native_turn = (RickyNativeTurn){.style=style <= 3 ? style : 0, .orientation=orientation, .from_right=from_right};
    s_next_native_region = (RickyNativeTurnRegion){0};
    s_next_reader_text_finish = false;
}

void epd_set_native_reader_text_finish(bool enabled) { s_next_reader_text_finish = enabled; }

void epd_set_native_ripple_origin(uint16_t x, uint16_t y) {
    if (s_next_native_turn.style != 3 || x > 1023 || y > 1023) return;
    s_next_native_turn.has_origin = true;
    s_next_native_turn.origin_x = x;
    s_next_native_turn.origin_y = y;
}

void epd_set_native_ui_turn(uint8_t orientation, bool from_right, int x, int y, int width, int height) {
    epd_set_native_turn(1, orientation, from_right);
    s_next_native_region = (RickyNativeTurnRegion){x, y, width, height, RICKY_UI_TURN_SPREAD};
}

// Align the end, not the start, of each native UI transition. Black/white UI
// strokes must not finish early and then wait for the long gray sequences.
// Move leading/trailing HOLD only; retain every active action, its order and
// every internal HOLD. Reader LUTs and the calibrated waveform are untouched.
static void ricky_ui_lock_completion(uint8_t* bank, int phases) {
    for (int pair = 0; pair < 256; ++pair) {
        int last = phases - 1;
        while (last >= 0 && bank[last * 1024 + pair * 4] == 0) --last;
        if (last < 0) continue; // unchanged/HOLD-only transitions stay inert
        // Retain the calibrated three final HOLD phases for settling; do not
        // trade those for an earlier animation. Unexpected tables stay as-is.
        // Plus UI uses the accepted reader's black-ink settling interval:
        // pure black lands four ticks before gray edges (seven final HOLDs).
        // Preserve every pulse and internal HOLD; do not add a refresh/phase.
        const int to = pair >> 4;
        const int finish = phases - (to == 0 ? 8 : 4);
        const int shift = finish - last;
        if (shift <= 0) continue;
        for (int tick = phases - 1; tick >= 0; --tick) {
            uint32_t action = 0;
            if (tick >= shift) memcpy(&action, bank + (tick - shift) * 1024 + pair * 4, sizeof(action));
            memcpy(bank + tick * 1024 + pair * 4, &action, sizeof(action));
        }
    }
}

static int ricky_ui_trim_idle_head(uint8_t* bank, int phases) {
    int head = 0;
    for (; head < phases; ++head) {
        bool active = false;
        for (int pair = 0; pair < 256; ++pair) {
            if (bank[head * 1024 + pair * 4]) { active = true; break; }
        }
        if (active) break;
    }
    if (head == 0 || head == phases) return phases;
    // All 256 transitions HOLD here, not just the currently visible pixels.
    // Omit only that global idle prefix; keep the three final settling HOLDs.
    memmove(bank, bank + head * 1024, (size_t)(phases - head) * 1024);
    return phases - head;
}

// Pure white-paper text: changing black/gray strokes finish near their gray edges.
// Move only exterior HOLD: keep every original active action and internal HOLD,
// at least three final settling ticks, unchanged-pixel cells (including the accepted
// retained-black re-drive). By default white transitions remain exact; the opt-in
// full-quality experiment below only moves their exterior HOLD earlier.
// This is a timing A/B experiment, not a claim of eliminating optical overshoot.
static void ricky_reader_lock_text_finish(uint8_t* bank, int phases) {
#if RICKYOS_PLUS_READER_EARLY_ERASE
    // Full GL16 erases retained black before it erases departing text. Move
    // only exterior HOLD of transitions to white so the departing glyph clears
    // before writing the new black. Keep every pulse/order/internal HOLD and
    // leave all black/gray targets byte-identical to the accepted quality bank.
    // white->white remains HOLD, including the unchanged-chrome sentinel.
    int black_start = 0;
    while (black_start < phases && bank[black_start * 1024 + 15 * 4] != 1) ++black_start;
    if (black_start > 0 && black_start < phases) {
        for (int from = 0; from < 15; ++from) {
            const int pair = 15 * 16 + from;
            int first = 0, last = phases - 1;
            while (first < phases && bank[first * 1024 + pair * 4] == 0) ++first;
            while (last >= first && bank[last * 1024 + pair * 4] == 0) --last;
            int shift = last - (black_start - 1);
            if (shift > first) shift = first;
            if (shift <= 0 || first == phases) continue;
            for (int tick = 0; tick < phases; ++tick) {
                uint32_t action = 0;
                if (tick + shift < phases)
                    memcpy(&action, bank + (tick + shift) * 1024 + pair * 4, sizeof(action));
                memcpy(bank + tick * 1024 + pair * 4, &action, sizeof(action));
            }
        }
    }
#endif
    for (int pair = 0; pair < 256; ++pair) {
        const int to = pair >> 4, from = pair & 15;
        if (to == 15 || to == from) continue;
        int last = phases - 1;
        while (last >= 0 && bank[last * 1024 + pair * 4] == 0) --last;
        if (last < 0) continue;
        // A/B after users liked convergence but reported lighter ink: let only
        // solid black finish four ticks before the gray edge (7 trailing HOLDs).
        // Same pulses/order/count, no additional drive, voltage or cycle ticks.
        const int finish = phases - (to == 0 ? 8 : 4);
        const int shift = finish - last;
        if (shift <= 0) continue;
        for (int tick = phases - 1; tick >= 0; --tick) {
            uint32_t action = 0;
            if (tick >= shift) memcpy(&action, bank + (tick - shift) * 1024 + pair * 4, sizeof(action));
            memcpy(bank + tick * 1024 + pair * 4, &action, sizeof(action));
        }
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKY_READER_ANY_OVERLAP
// Reader-motion optical experiment: keep departing text a little longer while
// new ink starts landing. Delay only exterior HOLD of transitions to white;
// never remove/reorder a pulse or internal HOLD, or touch black/gray targets.
// This reduces the erase-before-write gap without deleting drive actions.
// Actual drive duration still depends on phase cadence; verify optics on-panel.
// The 48-phase bank and its existing allocation are reused before feeders start.
static void ricky_reader_overlap_erase(uint8_t* bank, int phases, int overlap) {
    if (overlap <= 0) return;
    int black_start = 0;
    while (black_start < phases && bank[black_start * 1024 + 15 * 4] != 1) ++black_start;
    if (black_start == phases) return;
    int finish = black_start - 1 + overlap;
    if (finish > phases - 3) finish = phases - 3; // retain at least two final HOLDs
    for (int from = 0; from < 15; ++from) {
        const int pair = 15 * 16 + from;
        int last = phases - 1;
        while (last >= 0 && !bank[last * 1024 + pair * 4]) --last;
        const int shift = finish - last;
        if (last < 0 || shift <= 0) continue;
        for (int tick = phases - 1; tick >= 0; --tick) {
            uint32_t action = 0;
            if (tick >= shift)
                memcpy(&action, bank + (tick - shift) * 1024 + pair * 4, sizeof(action));
            memcpy(bank + tick * 1024 + pair * 4, &action, sizeof(action));
        }
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_EDGE_ERASE_WITH_CORE
// Early erase ends every transition to white together, so the longer gray->white
// sequences of old anti-aliased edges start before the black core: departing text
// first thins, then vanishes, and the page reads blank for longer (a visible flash
// with AA on). Start them with the core instead. Exterior HOLD moves only; every
// pulse and internal HOLD, and all black/gray targets, stay as they are. A
// sequence too long to start with the core moves as far as two final HOLDs allow.
static void ricky_reader_align_edge_erase(uint8_t* bank, int phases) {
    const int core = 15 * 16;  // black -> white
    int core_first = 0;
    while (core_first < phases && !bank[core_first * 1024 + core * 4]) ++core_first;
    if (core_first == phases) return;
    for (int from = 1; from < 15; ++from) {
        const int pair = 15 * 16 + from;
        int first = 0, last = phases - 1;
        while (first < phases && !bank[first * 1024 + pair * 4]) ++first;
        while (last >= first && !bank[last * 1024 + pair * 4]) --last;
        if (first == phases) continue;
        int shift = core_first - first;
        if (shift > phases - 3 - last) shift = phases - 3 - last;
        if (shift <= 0) continue;
        for (int tick = phases - 1; tick >= 0; --tick) {
            uint32_t action = 0;
            if (tick >= shift)
                memcpy(&action, bank + (tick - shift) * 1024 + pair * 4, sizeof(action));
            memcpy(bank + tick * 1024 + pair * 4, &action, sizeof(action));
        }
    }
}
#endif
#endif

static void ricky_native_end(RenderContext_t* ctx) {
    heap_caps_free(ctx->turn_luts);
    heap_caps_free(ctx->turn_delays);
#if !defined(CONFIG_IDF_TARGET_ESP32S31)
    heap_caps_free(ctx->turn_window);
#endif
    ctx->turn_luts = NULL;
    ctx->turn_delays = NULL;
    ctx->turn_window = NULL;
    ctx->turn_phases = 0;
    ctx->turn_window_tick = -1;
    ctx->turn_region = (RickyNativeTurnRegion){0};
    ctx->turn_producer_tick = 0;
    ctx->reader_text_finish = false;
}

static void ricky_native_begin(RenderContext_t* ctx, const EpdWaveformPhases* phases,
                               RickyNativeTurn turn) {
    ctx->turn_luts = NULL;
    ctx->turn_delays = NULL;
    ctx->turn_window = NULL;
    ctx->turn_phases = 0;
    ctx->turn_window_tick = -1;
    // Only differential full-screen native reader waves. Variable phase timing
    // cannot be staggered without changing each pixel's drive integral.
    const LutFunctionPair compact = find_lut_functions(ctx->mode, 1024);
    if ((!turn.style && !ctx->reader_text_finish) || turn.style > 3 || turn.orientation > 3 || !phases || phases->phases < 2 ||
        phases->phases > 64 || !(ctx->mode & MODE_PACKING_1PPB_DIFFERENCE) ||
        compact.build_func != ctx->lut_build_func || ctx->area.x || ctx->area.y ||
        ctx->area.width != ctx->display_width || ctx->area.height != ctx->display_height ||
        ctx->crop_to.x || ctx->crop_to.y ||
        ctx->crop_to.width != ctx->display_width || ctx->crop_to.height != ctx->display_height)
        return;
    if (phases->phase_times) {
        for (int i = 1; i < phases->phases; ++i)
            if (phases->phase_times[i] != phases->phase_times[0]) return;
    }
    const RickyNativeTurnRegion region = ctx->turn_region;
    if (region.spread && (region.spread > RICKY_UI_TURN_SPREAD || region.x < 0 || region.y < 0 ||
        region.width <= 0 || region.height <= 0 ||
        region.width > ctx->display_width || region.height > ctx->display_height ||
        region.x > ctx->display_width - region.width ||
        region.y > ctx->display_height - region.height)) return;
    const int cols = (ctx->display_width + 15) / 16;
    const int rows = (ctx->display_height + 15) / 16;
    // Optional, turn-scoped scratch; no extra framebuffer. Reading multiple hot
    // phase LUTs from PSRAM on every UI row misses the feeder deadline. Cache
    // only the simultaneously active phase window internally, not the whole bank.
    // S31 depads to256 B/phase: default9 slots2304 B; fine ripple24 slots6144 B.
    // Other targets retain their existing 1 KiB/phase converter. C owns scratch until both
    // feeders join; OOM falls back before any drive, never to a half animation.
    ctx->turn_luts = heap_caps_aligned_alloc(16, (size_t)phases->phases * 1024, MALLOC_CAP_SPIRAM);
    ctx->turn_delays = heap_caps_malloc((size_t)cols * rows, MALLOC_CAP_SPIRAM);
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    ctx->turn_window = s_native_turn_window;
#else
    ctx->turn_window = heap_caps_aligned_alloc(16, ((region.spread ? region.spread : 7) + 1) * 1024,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
#endif
    if (!ctx->turn_luts || !ctx->turn_delays || !ctx->turn_window) {
        ESP_LOGW("NativeTurn", "Scratch unavailable; ordinary refresh");
        ricky_native_end(ctx);
        return;
    }
    for (int i = 0; i < phases->phases; ++i)
        ctx->lut_build_func(ctx->turn_luts + i * 1024, phases, i);
    int nativePhases = phases->phases;
    if (region.spread) {
        ricky_ui_lock_completion(ctx->turn_luts, nativePhases);
        nativePhases = ricky_ui_trim_idle_head(ctx->turn_luts, nativePhases);
    } else if (ctx->reader_text_finish) {
        ricky_reader_lock_text_finish(ctx->turn_luts, nativePhases);
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKY_READER_ANY_OVERLAP
        // Plain turns too: the white gap between erase and write reads as a flash.
        if (turn.style <= 3)
            ricky_reader_overlap_erase(ctx->turn_luts, nativePhases,
                                       turn.style == 3 ? RICKYOS_PLUS_READER_RIPPLE_OVERLAP_PHASES
                                                       : RICKYOS_PLUS_READER_ERASE_OVERLAP_PHASES);
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_EDGE_ERASE_WITH_CORE
        if (turn.style <= 3) ricky_reader_align_edge_erase(ctx->turn_luts, nativePhases);
#endif
    }
    const RickyRippleGeometry ripple = turn.style == 3 ? ricky_ripple_geometry(turn,ctx->display_width,ctx->display_height)
                                                       : (RickyRippleGeometry){0};
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const int px = x * 16 + 8 < ctx->display_width ? x * 16 + 8 : ctx->display_width - 1;
            const int py = y * 16 + 8 < ctx->display_height ? y * 16 + 8 : ctx->display_height - 1;
            int delay;
            if (region.spread) {
                const int rx = px < region.x ? 0 :
                    px >= region.x + region.width ? region.width - 1 : px - region.x;
                const int ry = py < region.y ? 0 :
                    py >= region.y + region.height ? region.height - 1 : py - region.y;
                delay = ricky_ui_turn_delay(turn, rx, ry, region.width, region.height, region.spread);
            } else if (!turn.style) {
                delay = 0; // AA convergence is available with spatial motion OFF.
            } else {
                delay = turn.style == 3 ? ricky_ripple_delay(ricky_ripple_axis(ripple,px,py), ricky_reader_turn_spread(turn))
                                        : ricky_ripple_delay(ricky_turn_axis(turn, px, py, ctx->display_width, ctx->display_height),
                                                             ricky_reader_turn_spread(turn));
            }
            ctx->turn_delays[y * cols + x] = delay;
        }
    }
    ctx->turn_phases = nativePhases;
    ctx->turn_cols = cols;
    ctx->current_frame = 0;
    ctx->turn_producer_tick = 0;
    ctx->cycle_frames = nativePhases + (region.spread ? region.spread : ricky_reader_turn_spread(turn));
    ESP_LOGI("NativeTurn", "style=%u orientation=%u right=%d phases=%d ticks=%d scratch=%u",
             turn.style, turn.orientation, turn.from_right, ctx->turn_phases, ctx->cycle_frames,
             (unsigned)((size_t)phases->phases * 1024 + cols * rows));
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    ESP_LOGI("NativeTurn", "hot=%p cpu=%dMHz", (void*)ctx->turn_window, esp_clk_cpu_freq() / 1000000);
#endif
    if (ctx->reader_text_finish) ESP_LOGI("ReaderFinish", "completion-aligned text; phases=%d ticks=%d", nativePhases, ctx->cycle_frames);
}
