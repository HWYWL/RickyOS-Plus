/**
 * High-level API implementation for epdiy.
 */

#include <assert.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <string.h>

#include "epd_highlevel.h"
#include "epdiy.h"
#include "waveforms/my_waveform.h"

#ifndef _swap_int
#define _swap_int(a, b) \
    {                   \
        int t = a;      \
        a = b;          \
        b = t;          \
    }
#endif

static bool already_initialized = 0;

EpdiyHighlevelState epd_hl_init(const EpdWaveform* waveform) {
    assert(!already_initialized);
    if (waveform == NULL) {
        waveform = epd_get_display()->default_waveform;
    }

    int fb_size = epd_width() / 2 * epd_height();

#if !(defined(CONFIG_ESP32_SPIRAM_SUPPORT) || defined(CONFIG_ESP32S3_SPIRAM_SUPPORT) || \
      defined(CONFIG_SPIRAM))
    ESP_LOGW(
        "EPDiy", "Please enable PSRAM (menuconfig → Component config → ESP PSRAM)"
    );
#endif
    /* S31 LCD 走 AXI-GDMA，PSRAM 传输对齐 64B；16B 会对齐失败并 abort */
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
    const size_t fb_align = 64;
#else
    const size_t fb_align = 16;
#endif
    EpdiyHighlevelState state;
    state.back_fb = heap_caps_aligned_alloc(fb_align, fb_size, MALLOC_CAP_SPIRAM);
    assert(state.back_fb != NULL);
    state.front_fb = heap_caps_aligned_alloc(fb_align, fb_size, MALLOC_CAP_SPIRAM);
    assert(state.front_fb != NULL);
    state.difference_fb = heap_caps_aligned_alloc(fb_align, 2 * fb_size, MALLOC_CAP_SPIRAM);
    assert(state.difference_fb != NULL);
    state.dirty_lines = malloc(epd_height() * sizeof(bool));
    assert(state.dirty_lines != NULL);
    state.dirty_columns
        = heap_caps_aligned_alloc(16, epd_width() / 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(state.dirty_columns != NULL);
    state.waveform = waveform;

    ESP_LOGW("epd_dram",
             "hl FB in PSRAM: front+back+diff=%u KB; dirty_cols INTERNAL=%u B",
             (unsigned)((fb_size * 4) / 1024), (unsigned)(epd_width() / 2));

    memset(state.front_fb, 0xFF, fb_size);
    memset(state.back_fb, 0xFF, fb_size);

    already_initialized = true;
    return state;
}

uint8_t* epd_hl_get_framebuffer(EpdiyHighlevelState* state) {
    assert(state != NULL);
    return state->front_fb;
}

uint8_t* epd_hl_get_back_framebuffer(EpdiyHighlevelState* state) {
    assert(state != NULL);
    return state->back_fb;
}

enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature) {
    return epd_hl_update_area(state, mode, temperature, epd_full_screen());
}

static void hold_equal_reader_pixels(uint8_t* row, int begin, int end) {
    for (int x = begin; x < end; ++x) {
        if ((row[x] & 15) == (row[x] >> 4)) row[x] = 0xff;
    }
}

static enum EpdDrawError update_screen_full(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature, const EpdRect* reader_body
) {
    assert(state != NULL);

    EpdRect area = epd_full_screen();
    uint32_t ts = esp_timer_get_time() / 1000;

    epd_difference_image_cropped(
        state->front_fb,
        state->back_fb,
        area,
        state->difference_fb,
        state->dirty_lines,
        state->dirty_columns
    );

    if (reader_body != NULL) {
        // The reader table re-drives retained black to prevent faint text. Hold
        // unchanged chrome instead, in the SAME scan: encode its equal old/new
        // levels as white->white, whose reader waveform is entirely no-drive.
        // Changed page numbers/time/battery keep their real transitions. Never
        // alter front/back pixels or use this mask with a cleanup waveform.
        for (int y = 0; y < epd_height(); ++y) {
            uint8_t* row = state->difference_fb + (size_t)y * epd_width();
            const bool body_row = y >= reader_body->y && y < reader_body->y + reader_body->height;
            // Do not walk the body pixels a second time (usually most of the panel).
            hold_equal_reader_pixels(row, 0, body_row ? reader_body->x : epd_width());
            hold_equal_reader_pixels(row, body_row ? reader_body->x + reader_body->width : epd_width(), epd_width());
        }
    }

    int fb_height = epd_height();
    int col_bytes = epd_width() / 2;
    for (int y = 0; y < fb_height; y++) {
        state->dirty_lines[y] = true;
    }
    memset(state->dirty_columns, 0xFF, col_bytes);

    uint32_t t1 = esp_timer_get_time() / 1000;

    EpdRect full = epd_full_screen();
    ESP_LOGI(
        "epd_path",
        "[upd] full draw_base packing=1PPB|mode=0x%X area=%dx%d diff_fb=%p",
        (unsigned)mode,
        full.width,
        full.height,
        (void*)state->difference_fb
    );
    enum EpdDrawError err = epd_draw_base(
        full,
        state->difference_fb,
        full,
        MODE_PACKING_1PPB_DIFFERENCE | mode,
        temperature,
        state->dirty_lines,
        state->dirty_columns,
        state->waveform
    );

    uint32_t t2 = esp_timer_get_time() / 1000;

    int fb_size = col_bytes * fb_height;
    if (err == EPD_DRAW_SUCCESS) memcpy(state->back_fb, state->front_fb, fb_size);

    uint32_t t3 = esp_timer_get_time() / 1000;

    ESP_LOGI(
        "epdiy",
        "full diff: %dms, draw: %dms, buffer update: %dms, total: %dms",
        t1 - ts,
        t2 - t1,
        t3 - t2,
        t3 - ts
    );
    return err;
}

enum EpdDrawError epd_hl_update_screen_full(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature
) {
    return update_screen_full(state, mode, temperature, NULL);
}

enum EpdDrawError epd_hl_update_screen_reader(
    EpdiyHighlevelState* state, int temperature, EpdRect body
) {
    // Physical coordinates, deliberately not EPDiy rotation: GfxRenderer has
    // already mapped the current reading layout. Bad/empty hints fail safe.
    const bool valid = body.x >= 0 && body.y >= 0 && body.width > 0 && body.height > 0 &&
        body.x <= epd_width() && body.y <= epd_height() &&
        body.width <= epd_width() - body.x && body.height <= epd_height() - body.y;
    return update_screen_full(state, MODE_GC16_FAST, temperature, valid ? &body : NULL);
}

enum EpdDrawError epd_hl_update_screen_reader_paper(
    EpdiyHighlevelState* state, int temperature, EpdRect body
) {
    // Same one-scan chrome mask, but paper grays retain their erase pulses.
    const bool valid = body.x >= 0 && body.y >= 0 && body.width > 0 && body.height > 0 &&
        body.x <= epd_width() && body.y <= epd_height() &&
        body.width <= epd_width() - body.x && body.height <= epd_height() - body.y;
    return update_screen_full(state, (enum EpdDrawMode)MY_MODE_READER_PAPER_TURN, temperature, valid ? &body : NULL);
}

enum EpdDrawError epd_hl_update_screen_from_white(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature
) {
    assert(state != NULL);
    int fb_size = epd_width() / 2 * epd_height();
    ESP_LOGI(
        "epd_path",
        "[upd] from_white mode=0x%X temp=%d fb=%dx%d/%2 wf=%p front=%p back=%p",
        (unsigned)mode,
        temperature,
        epd_width(),
        epd_height(),
        (void*)state->waveform,
        (void*)state->front_fb,
        (void*)state->back_fb
    );
    memset(state->back_fb, 0xFF, fb_size);
    return epd_hl_update_screen_full(state, mode, temperature);
}

EpdRect _inverse_rotated_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    // If partial update uses full screen do not rotate anything
    if (!(x == 0 && y == 0 && epd_width() == w && epd_height() == h)) {
        // invert the current display rotation
        switch (epd_get_rotation()) {
            // 0 landscape: Leave it as is
            case EPD_ROT_LANDSCAPE:
                break;
            // 1 90 ° clockwise
            case EPD_ROT_PORTRAIT:
                _swap_int(x, y);
                _swap_int(w, h);
                x = epd_width() - x - w;
                break;

            case EPD_ROT_INVERTED_LANDSCAPE:
                // 3 180°
                x = epd_width() - x - w;
                y = epd_height() - y - h;
                break;

            case EPD_ROT_INVERTED_PORTRAIT:
                // 3 270 °
                _swap_int(x, y);
                _swap_int(w, h);
                y = epd_height() - y - h;
                break;
        }
    }

    EpdRect rotated = { x, y, w, h };
    return rotated;
}

// Plus regional transactions: mask columns outside the 32-pixel aligned area,
// and publish only pixels physically submitted after a successful waveform.
static enum EpdDrawError update_region(EpdiyHighlevelState* state, enum EpdDrawMode mode,
                                      int temperature, EpdRect area, bool cleanup) {
    assert(state != NULL);
    area = _inverse_rotated_area(area.x, area.y, area.width, area.height);
    int x0 = area.x < 0 ? 0 : area.x;
    int y0 = area.y < 0 ? 0 : area.y;
    int x1 = area.x + area.width;
    int y1 = area.y + area.height;
    if (x1 > epd_width()) x1 = epd_width();
    if (y1 > epd_height()) y1 = epd_height();
    if (x1 <= x0 || y1 <= y0) return EPD_DRAW_SUCCESS;
    x0 &= ~31;
    x1 = (x1 + 31) & ~31;
    if (x1 > epd_width()) x1 = epd_width();
    area = (EpdRect){x0, y0, x1-x0, y1-y0};
    EpdRect diff = epd_difference_image_cropped(state->front_fb, state->back_fb, area,
                            state->difference_fb, state->dirty_lines, state->dirty_columns);
    if (!cleanup && (diff.width == 0 || diff.height == 0)) return EPD_DRAW_SUCCESS;
    // The vendor difference routine interlaces whole rows. Its outside-column
    // dirtiness must never drive the other policy zone.
    memset(state->dirty_columns, 0, x0 / 2);
    memset(state->dirty_columns + x1 / 2, 0, (epd_width() - x1) / 2);
    if (cleanup) {
        memset(state->dirty_columns + x0 / 2, 0xff, (x1-x0) / 2);
        for (int y=y0; y<y1; ++y) state->dirty_lines[y] = true;
    }
    // Vendor get_buffer_params shifts the input pointer for a nonzero crop x,
    // while the LCD kernel still decodes a whole physical row. Keep the full
    // raster address/stride; dirty lines and masked columns define the region.
    enum EpdDrawError err = epd_draw_base(epd_full_screen(), state->difference_fb, epd_full_screen(),
                 MODE_PACKING_1PPB_DIFFERENCE | mode, temperature, state->dirty_lines,
                 state->dirty_columns, state->waveform);
    if (err == EPD_DRAW_SUCCESS) {
        for (int y=y0; y<y1; ++y) {
            if (!state->dirty_lines[y]) continue;
            size_t offset = (size_t)y * epd_width() / 2 + x0 / 2;
            memcpy(state->back_fb + offset, state->front_fb + offset, (x1-x0) / 2);
        }
    }
    return err;
}

enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState* state, enum EpdDrawMode mode,
                                   int temperature, EpdRect area) {
    return update_region(state, mode, temperature, area, false);
}

enum EpdDrawError epd_hl_update_area_full(EpdiyHighlevelState* state, enum EpdDrawMode mode,
                                        int temperature, EpdRect area) {
    return update_region(state, mode, temperature, area, true);
}

void epd_hl_set_all_white(EpdiyHighlevelState* state) {
    assert(state != NULL);
    int fb_size = epd_width() / 2 * epd_height();
    memset(state->front_fb, 0xFF, fb_size);
}

void epd_fullclear(EpdiyHighlevelState* state, int temperature) {
    assert(state != NULL);
    epd_hl_set_all_white(state);
    enum EpdDrawError err = epd_hl_update_screen(state, MODE_GC16, temperature);
    assert(err == EPD_DRAW_SUCCESS);
    epd_clear();
}

void epd_hl_waveform(EpdiyHighlevelState* state, const EpdWaveform* waveform) {
    if (waveform == NULL) {
        waveform = epd_get_display()->default_waveform;
    }
    state->waveform = waveform;
}
