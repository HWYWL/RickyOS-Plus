/**
 * @brief MY_WAVEFORM：完整 LUT 头 + 可选开机裁剪（对齐 pico E0470_WAVEFORM）
 */

#include "my_waveform.h"

#include "my_waveform_du.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_trim.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <esp_log.h>

#define TAG "my_wf"

// 0=完整 48 相；1=开机裁成 GC16 36 / GL16 37（pico 默认）
#ifndef MY_WAVEFORM_TRIM
#define MY_WAVEFORM_TRIM 0
#endif

#define MY_FULL_DU_FRAMES 20
#define MY_FULL_GC16_FRAMES 48
#define MY_FULL_GL16_FRAMES 48
#ifndef RICKYOS_PLUS_READER_QUALITY
#define RICKYOS_PLUS_READER_QUALITY 0
#endif
#if RICKYOS_PLUS_READER_QUALITY != 0 && RICKYOS_PLUS_READER_QUALITY != 1
#error "Reader quality experiment must be 0 or 1"
#endif
#define MY_TRIM_ERASE_MAX 11
#define MY_TRIM_SAT_CUT 5
#define MY_TRIM_WHITE_SAT_CUT 0
#define MY_TRIM_HOLD 3
// Opt-in UI gray-floor experiment. Reuse GL16's existing white maintenance
// action after reader/paper tables have been copied. No additional scan/array.
#ifndef RICKYOS_PLUS_UI_WHITE_MAINTENANCE
#define RICKYOS_PLUS_UI_WHITE_MAINTENANCE 0
#endif
#if RICKYOS_PLUS_UI_WHITE_MAINTENANCE != 0 && RICKYOS_PLUS_UI_WHITE_MAINTENANCE != 1
#error "UI white maintenance must be 0 or 1"
#endif
// UI-only optical experiment: restore part of the five discarded black
// saturation phases. Reader tables are copied BEFORE this adjustment.
#ifndef RICKYOS_PLUS_UI_BLACK_RESTORE_PHASES
#define RICKYOS_PLUS_UI_BLACK_RESTORE_PHASES 2
#endif
#if RICKYOS_PLUS_UI_BLACK_RESTORE_PHASES < 0 || RICKYOS_PLUS_UI_BLACK_RESTORE_PHASES > MY_TRIM_SAT_CUT
#error "UI black restoration must remain within the original saturation margin"
#endif
// Opt-in reader ink experiment; default reproduces the released reader.
#ifndef RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES
#define RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES 0
#endif
#if RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES < 0 || RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES > MY_TRIM_SAT_CUT
#error "Reader black restoration must remain within the original saturation margin"
#endif
// A normal layout build must not silently include the unaccepted short-waveform
// experiment. Keep it opt-in, and retain every changing-pixel pulse by default.
#ifndef RICKYOS_PLUS_READER_SHORT_WAVEFORM
#define RICKYOS_PLUS_READER_SHORT_WAVEFORM 0
#endif
#if RICKYOS_PLUS_READER_QUALITY && RICKYOS_PLUS_READER_SHORT_WAVEFORM
#error "Reader quality requires the full source waveform"
#endif
#if RICKYOS_PLUS_READER_SHORT_WAVEFORM
#define MY_READER_GL16_FRAMES 37
#else
#define MY_READER_GL16_FRAMES MY_FULL_GL16_FRAMES
#endif
#if MY_WAVEFORM_TRIM
#define MY_GC16_FRAMES 36
#define MY_GL16_FRAMES 37
#else
#define MY_GC16_FRAMES MY_FULL_GC16_FRAMES
#define MY_GL16_FRAMES MY_FULL_GL16_FRAMES
#endif

static const EpdWaveformTempInterval my_waveform_intervals[] = {
    { .min = 0, .max = 50 },
};

static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

// 源 DU 只认目标 0/15；中间灰 50/50，暗→黑、亮→白
static uint8_t s_du_data[MY_FULL_DU_FRAMES][16][4];
static const EpdWaveformPhases s_du_phases = {
    .phases = MY_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_du_data[0],
};
static const EpdWaveformPhases* s_du_ranges[] = { &s_du_phases };
static const EpdWaveformMode s_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &s_du_ranges[0],
};

static void complete_du_build(void) {
    memset(s_du_data, 0, sizeof(s_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < MY_FULL_DU_FRAMES; f++) {
                const int action = lut_get(my_du_25_0_data, f, to_bin, from);
                if (action != 0) {
                    lut_or(s_du_data, f, to, from, action);
                }
            }
        }
    }
}

// 白底 15→15 源表全保持：挂在已有往白推的一相上再推 1 帧
static void gl16_white_tick(uint8_t (*data)[16][4], int frames) {
    int tick = -1;
    for (int f = frames - 1; f >= 0; f--) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                tick = f;
                break;
            }
        }
        if (tick >= 0) {
            break;
        }
    }
    if (tick < 0) {
        tick = frames > 2 ? frames - 3 : 0;
    }
    lut_or(data, tick, 15, 15, 2);
}

static uint8_t s_gc16_data[MY_FULL_GC16_FRAMES][16][4];
static uint8_t s_gl16_data[MY_FULL_GL16_FRAMES][16][4];
// Default: full 48-phase source, unchanged pixels held. Optional short experiment:
// 37 phases, still requires optical acceptance. Static 3072/2368-byte LUT only;
// neither path allocates a framebuffer or changes voltage / line timing.
static uint8_t s_reader_gl16_data[MY_READER_GL16_FRAMES][16][4];
static const EpdWaveformPhases s_gc16_phases = {
    .phases = MY_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gc16_data[0],
};
static const EpdWaveformPhases s_gl16_phases = {
    .phases = MY_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gl16_data[0],
};
static const EpdWaveformPhases* s_gc16_ranges[] = { &s_gc16_phases };
static const EpdWaveformPhases* s_gl16_ranges[] = { &s_gl16_phases };
static const EpdWaveformMode s_gc16_mode = {
    .type = 2,
    .temp_ranges = 1,
    .range_data = &s_gc16_ranges[0],
};
static const EpdWaveformMode s_gl16_mode = {
    .type = 5,
    .temp_ranges = 1,
    .range_data = &s_gl16_ranges[0],
};
static const EpdWaveformPhases s_reader_gl16_phases = {
    .phases = MY_READER_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_reader_gl16_data[0],
};
static const EpdWaveformPhases* s_reader_gl16_ranges[] = { &s_reader_gl16_phases };
static const EpdWaveformMode s_reader_gl16_mode = {
    // Existing EPDiy mode identifier for the reader-only short experiment.
    .type = 0x6,  // MODE_GL16_FAST in epdiy.h
    .temp_ranges = 1,
    .range_data = &s_reader_gl16_ranges[0],
};

// UI turn (tab switches and other regional UI updates), the Read Pico's text-turn table:
// trimmed GL16, every (to, from) sequence left-aligned so erasing the old and drawing the
// new start together (a cross-fade, no white field), unchanged pixels not driven, and a
// pixel turning black pushed straight to black instead of erasing to white first. The
// trim's shorter erase leaves a moved marker faintly visible; regular GC16 cleanups of
// the regional ledger clear it.
#define MY_UI_TURN_FRAMES 37
static uint8_t s_ui_turn_data[MY_UI_TURN_FRAMES][16][4];
static const EpdWaveformPhases s_ui_turn_phases = {
    .phases = MY_UI_TURN_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_ui_turn_data[0],
};
static const EpdWaveformPhases* s_ui_turn_ranges[] = { &s_ui_turn_phases };
static const EpdWaveformMode s_ui_turn_mode = {
    .type = 0xB,  // MODE_GL16_INV in epdiy.h, otherwise unused here: MY_MODE_UI_TURN
    .temp_ranges = 1,
    .range_data = &s_ui_turn_ranges[0],
};

// Reader turn: the UI-turn table (the Read Pico's text turn: trimmed GL16, left-aligned,
// unchanged pixels held, new black written without erasing first), so a page lands at
// once, plus a short balanced re-drive of retained black (to == from == 0): a few erase
// phases, then as many darkening ones. Fully held, black strokes that stay across turns
// faded; the vendor's full 18 + 18 erase and rewrite kept them dark but read as a blink.
#define MY_READER_TURN_FRAMES MY_UI_TURN_FRAMES
// Reuse the gallery's 2368-byte erase snapshot as its final paper table. The
// ordinary reader table remains byte-identical to the accepted white-page build;
// no extra frame, heap allocation or waveform bank beyond the gallery budget.
static uint8_t s_paper_turn_data[MY_UI_TURN_FRAMES][16][4];
#define MY_READER_PAPER_GRAY 8  // reading-background paper sits at or above this level
// Retained-black optical experiment. Default keeps the accepted 5+5 re-drive.
// At most 17+17 fits this 37-phase bank with the final three HOLDs intact, and
// stays within the full GC/GL black-cell erase and darkening pulse counts.
#ifndef RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE
#define RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE 5
#endif
#if RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE < 5 || \
    RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE > ((MY_READER_TURN_FRAMES - MY_TRIM_HOLD) / 2)
#error "Retained black re-drive must preserve the three-phase settling tail"
#endif
#define MY_READER_TURN_PULSE RICKYOS_PLUS_READER_RETAINED_BLACK_PULSE
static uint8_t s_turn_data[MY_READER_TURN_FRAMES][16][4];
static const EpdWaveformPhases s_turn_phases = {
    .phases = MY_READER_TURN_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_turn_data[0],
};
static const EpdWaveformPhases* s_turn_ranges[] = { &s_turn_phases };
static const EpdWaveformMode s_turn_mode = {
    .type = 0x3,  // MODE_GC16_FAST in epdiy.h, otherwise unused here: MY_MODE_READER_TURN
    .temp_ranges = 1,
    .range_data = &s_turn_ranges[0],
};
static const EpdWaveformPhases s_paper_turn_phases = {
    .phases = MY_READER_TURN_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_paper_turn_data[0],
};
static const EpdWaveformPhases* s_paper_turn_ranges[] = { &s_paper_turn_phases };
static const EpdWaveformMode s_paper_turn_mode = {
    .type = MY_MODE_READER_PAPER_TURN,
    .temp_ranges = 1,
    .range_data = &s_paper_turn_ranges[0],
};

static const EpdWaveformMode* s_modes[] = {
    &s_du_mode,
    &s_gc16_mode,
    &s_gl16_mode,
    &s_reader_gl16_mode,
    &s_ui_turn_mode,
    &s_turn_mode,
    &s_paper_turn_mode,
};

const EpdWaveform MY_WAVEFORM = {
    .num_modes = sizeof(s_modes) / sizeof(s_modes[0]),
    .num_temp_ranges = 1,
    .mode_data = s_modes,
    .temp_intervals = my_waveform_intervals,
};

static void left_align(const uint8_t (*src)[16][4], int frames, uint8_t (*dst)[16][4]) {
    memset(dst, 0, (size_t)frames * 16 * 4);
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            uint8_t seq[64];
            for (int f = 0; f < frames; f++) seq[f] = (uint8_t)lut_get(src, f, to, from);
            int a = 0;
            while (a < frames && seq[a] == 0) a++;
            int z = frames;
            while (z > a && seq[z - 1] == 0) z--;
            for (int f = a; f < z; f++) lut_or(dst, f - a, to, from, seq[f]);
        }
    }
}

// Built in the GL scratch (the trim needs room for the full source), which the caller
// restores afterwards.
static void ui_turn_build(void) {
    const my_waveform_trim_t trim = {
        .erase_max = MY_TRIM_ERASE_MAX,
        .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT,
        .hold = MY_TRIM_HOLD,
    };
    const int frames = my_waveform_trim(&my_gl_25_0, &trim, s_gl16_data);
    assert(frames == MY_UI_TURN_FRAMES);
    gl16_white_tick(s_gl16_data, frames);
    left_align((const uint8_t (*)[16][4])s_gl16_data, MY_UI_TURN_FRAMES, s_ui_turn_data);
    for (int f = 0; f < MY_UI_TURN_FRAMES; f++) {
        for (int v = 0; v < 16; v++) {
            s_ui_turn_data[f][v][v / 4] &= (uint8_t)~(3u << (6 - 2 * (v % 4)));
        }
    }
    memcpy(s_paper_turn_data, s_ui_turn_data, sizeof(s_paper_turn_data));
    // Target black (to != 15): drop the leading erase-to-white (action 2) so the new mark
    // goes straight to black. Target white keeps its erase: that is what clears the old.
    for (int to = 0; to < 15; to++) {
        for (int from = 0; from < 16; from++) {
            int f = 0;
            while (f < MY_UI_TURN_FRAMES && lut_get((const uint8_t (*)[16][4])s_ui_turn_data, f, to, from) == 2) {
                s_ui_turn_data[f][to][from / 4] &= (uint8_t)~(3u << (6 - 2 * (from % 4)));
                ++f;
            }
        }
    }
}

static void restore_black_margin(uint8_t (*data)[16][4], int frames, int limit, bool reader) {
    // Only changing pixels whose final target is pure black. Do not touch
    // unchanged cells, gray antialias edges or white erases.
    // No added scan: replace trailing HOLD within this existing 37-phase bank,
    // leaving at least the calibrated three settling phases. Never exceed the
    // original full GL16 table's darkening count for the same transition.
    for (int from = 1; from < 16; ++from) {
        int original_dark = 0, clean_dark = 0, current_dark = 0, last = -1;
        for (int f = 0; f < MY_FULL_GL16_FRAMES; ++f) {
            original_dark += lut_get(my_gl_25_0_data, f, 0, from) == 1;
            clean_dark += lut_get(my_gc_25_0_data, f, 0, from) == 1;
        }
        if (reader && clean_dark < original_dark) original_dark = clean_dark;
        for (int f = 0; f < frames; ++f) {
            const int action = lut_get((const uint8_t (*)[16][4])data, f, 0, from);
            current_dark += action == 1;
            if (action) last = f;
        }
        if (last < 0) continue;
        const int available = frames - MY_TRIM_HOLD - last - 1;
        int restore = original_dark - current_dark;
        if (restore > limit) restore = limit;
        if (restore > available) restore = available;
        for (int f = 1; f <= restore; ++f) lut_or(data, last + f, 0, from, 1);
    }
}

void my_waveform_init(void) {
    complete_du_build();
    ui_turn_build();

#if MY_WAVEFORM_TRIM
    const my_waveform_trim_t trim = {
        .erase_max = MY_TRIM_ERASE_MAX,
        .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT,
        .hold = MY_TRIM_HOLD,
    };
    const int gc = my_waveform_trim(&my_gc_25_0, &trim, s_gc16_data);
    const int gl = my_waveform_trim(&my_gl_25_0, &trim, s_gl16_data);
    assert(gc == MY_GC16_FRAMES);
    assert(gl == MY_GL16_FRAMES);
    ESP_LOGD(TAG, "trimmed GC16=%d GL16=%d DU=%d (from full 48/48/20)", gc, gl, MY_FULL_DU_FRAMES);
#else
    memcpy(s_gc16_data, my_gc_25_0_data, sizeof(s_gc16_data));
    memcpy(s_gl16_data, my_gl_25_0_data, sizeof(s_gl16_data));
    ESP_LOGD(TAG, "full GC16=%d GL16=%d DU=%d (trim off)", MY_GC16_FRAMES, MY_GL16_FRAMES,
             MY_FULL_DU_FRAMES);
#endif
    // Full and trimmed GL16 both need the unchanged-white background pulse.
    gl16_white_tick(s_gl16_data, MY_GL16_FRAMES);
#if RICKYOS_PLUS_READER_SHORT_WAVEFORM
    const my_waveform_trim_t reader_trim = {
        .erase_max = MY_TRIM_ERASE_MAX,
        .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT,
        .hold = MY_TRIM_HOLD,
    };
    // trim's destination contract requires space for the full source. Reuse
    // the existing full-size GL scratch at boot, then restore it byte-for-byte.
    // No new 48-phase staging buffer or heap allocation.
    const int reader_frames = my_waveform_trim(&my_gl_25_0, &reader_trim, s_gl16_data);
    assert(reader_frames == MY_READER_GL16_FRAMES);
    memcpy(s_reader_gl16_data, s_gl16_data, sizeof(s_reader_gl16_data));
#if MY_WAVEFORM_TRIM
    const int restored = my_waveform_trim(&my_gl_25_0, &reader_trim, s_gl16_data);
    assert(restored == MY_GL16_FRAMES);
#else
    memcpy(s_gl16_data, my_gl_25_0_data, sizeof(s_gl16_data));
#endif
    gl16_white_tick(s_gl16_data, MY_GL16_FRAMES);
#else
    memcpy(s_reader_gl16_data, my_gl_25_0_data, sizeof(s_reader_gl16_data));
#endif
#if !RICKYOS_PLUS_READER_QUALITY
    for (int f = 0; f < MY_READER_GL16_FRAMES; ++f) {
        for (int gray = 0; gray < 16; ++gray) {
            s_reader_gl16_data[f][gray][gray / 4] &=
                (uint8_t)~(3u << (6 - 2 * (gray % 4)));
        }
    }
#else
    // Reuse the already-reserved 48-phase reader bank verbatim. Full GL16's
    // white->white cell is HOLD, so the unchanged-chrome mask remains valid;
    // every other source transition, including retained ink, keeps its timing.
    // No additional table/framebuffer. Native scratch can grow in PSRAM only.
    s_turn_ranges[0] = &s_reader_gl16_phases;
    s_paper_turn_ranges[0] = &s_reader_gl16_phases;
#endif
    memset(s_turn_data, 0, sizeof(s_turn_data));
    memcpy(s_turn_data, s_ui_turn_data, sizeof(s_ui_turn_data));
    // Under a reading background the paper is a light gray, not white. The old text
    // turning back into it is a light target, which must be erased first like white;
    // without the erase it went black before settling (a dark flash where the
    // previous page's text was). Ink and dark anti-aliasing keep the straight path.
    for (int f = 0; f < MY_READER_TURN_FRAMES; f++) {
        for (int to = 0; to < 16; to++) {
            if (to < MY_READER_PAPER_GRAY || to == 15) {
                memcpy(s_paper_turn_data[f][to], s_turn_data[f][to], sizeof(s_paper_turn_data[f][to]));
            }
        }
    }
    for (int f = 0; f < MY_READER_TURN_PULSE; f++) {
        lut_or(s_turn_data, f, 0, 0, 2);
        lut_or(s_turn_data, f + MY_READER_TURN_PULSE, 0, 0, 1);
        lut_or(s_paper_turn_data, f, 0, 0, 2);
        lut_or(s_paper_turn_data, f + MY_READER_TURN_PULSE, 0, 0, 1);
    }
    // Reading black restoration is opt-in; its default preserves the released
    // tables. Image banks remain unchanged, including in the experiment.
    restore_black_margin(s_turn_data, MY_READER_TURN_FRAMES, RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES, true);
    restore_black_margin(s_paper_turn_data, MY_READER_TURN_FRAMES, RICKYOS_PLUS_READER_BLACK_RESTORE_PHASES, true);
    restore_black_margin(s_ui_turn_data, MY_UI_TURN_FRAMES, RICKYOS_PLUS_UI_BLACK_RESTORE_PHASES, false);
#if RICKYOS_PLUS_UI_WHITE_MAINTENANCE
    // ui_turn_build deliberately held every diagonal cell, also removing the
    // already-present GL16 white->white maintenance action. Restore ONLY that
    // action in the UI bank. Do this last so neither reading nor paper inherits
    // it. Changed gray/black/white sequences and the 37-phase budget stay intact.
    gl16_white_tick(s_ui_turn_data, MY_UI_TURN_FRAMES);
#endif
    ESP_LOGD(TAG, "reader GL16=%d (short experiment=%d)",
             MY_READER_GL16_FRAMES, RICKYOS_PLUS_READER_SHORT_WAVEFORM);
}
