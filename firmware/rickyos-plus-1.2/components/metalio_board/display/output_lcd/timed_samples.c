// Private board calibration: charge a small row window, then explicitly scan
// HOLD at a measured deadline. HOLD is a source code, not proof of zero field.
#include "../output_common/render_method.h"
#if defined(RENDER_METHOD_LCD) && RICKYOS_PLUS_GRAY_CALIBRATION
#include "render_lcd.h"
#include "lcd_driver.h"
#include "epd_board.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

// Gray CODE, not calibrated optical gray. White stays untouched. Maximum
// 150ms is below the previous ~250ms 18-frame dark run. No reverse pulse.
static const int sample_durations_us[16] = {
    150000,95000,65000,45000,30000,20000,14000,10000,
    8000,6500,5500,4500,4000,3500,3000,0,
};

static bool IRAM_ATTR timed_line_isr(RenderContext_t* ctx, uint8_t* row) {
    if (ctx->lines_consumed < ctx->area.height)
        memcpy(row, ctx->static_line_buffer, ctx->display_width / 4);
    else memset(row, 0, ctx->display_width / 4); // At least four trailing HOLD rows.
    ++ctx->lines_consumed;
    return false;
}

static void IRAM_ATTR timed_done_isr(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);
    if (ctx->lines_consumed != ctx->lines_total) ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
    BaseType_t awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &awoken);
    portYIELD_FROM_ISR();
}

static bool timed_scan(RenderContext_t* ctx, EpdRect area, bool dark, int64_t at, int64_t* started) {
    // Borrow an existing worker row while both workers are idle. No allocation.
    memset(ctx->static_line_buffer, 0, ctx->display_width / 4);
    if (dark) for (int x = area.x; x < area.x + area.width; ++x)
        ctx->static_line_buffer[x / 4] |= (uint8_t)(1u << (6 - 2 * (x % 4)));
    const int padded = ((area.height + 4 + 7) / 8) * 8;
    if (!epd_lcd_set_scan_window(area.y, padded)) {
        ctx->error |= EPD_DRAW_INVALID_CROP;
        return false;
    }
    ctx->area = area;
    ctx->lines_consumed = 0;
    ctx->lines_total = padded;
    (void)xSemaphoreTake(ctx->frame_done, 0);
    epd_lcd_frame_done_cb((frame_done_func_t)timed_done_isr, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)timed_line_isr, ctx);
    *started = epd_lcd_start_frame_at(at);
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(30)) != pdTRUE) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        epd_lcd_line_source_cb(NULL, NULL);
        epd_lcd_frame_done_cb(NULL, NULL);
        epd_lcd_abort_frame();
        ESP_LOGE("EpdTiming", "timed sample scan timeout");
    }
    return ctx->error == EPD_DRAW_SUCCESS;
}

static bool timed_full_hold(RenderContext_t* ctx) {
    memset(ctx->static_line_buffer, 0, ctx->display_width / 4);
    if (!epd_lcd_set_scan_window(0, 0)) {
        ctx->error |= EPD_DRAW_INVALID_CROP;
        return false;
    }
    ctx->area = (EpdRect){0,0,ctx->display_width,0};
    ctx->lines_consumed = 0;
    ctx->lines_total = ((ctx->display_height + 7) / 8) * 8;
    (void)xSemaphoreTake(ctx->frame_done, 0);
    epd_lcd_frame_done_cb((frame_done_func_t)timed_done_isr, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)timed_line_isr, ctx);
    epd_lcd_start_frame();
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(30)) != pdTRUE) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        epd_lcd_line_source_cb(NULL,NULL);
        epd_lcd_frame_done_cb(NULL,NULL);
        epd_lcd_abort_frame();
    }
    return ctx->error == EPD_DRAW_SUCCESS;
}

enum EpdDrawError epd_timed_samples_lcd(RenderContext_t* ctx, const EpdRect* cells, int count) {
    if (!cells || count != 16 || !ctx->feed_line_buffers[0]) return EPD_DRAW_INVALID_CROP;
    // Validate every window before touching shared scratch or the panel.
    for (int i = 0; i < count; ++i) {
        const EpdRect a = cells[i];
        if (a.x < 0 || a.width <= 0 || a.width > ctx->display_width
            || a.x > ctx->display_width - a.width || a.y < 0 || a.height <= 0
            || a.height > ctx->display_height || a.y > ctx->display_height - a.height
            || a.y + ((a.height + 11) / 8) * 8 > ctx->display_height)
            return EPD_DRAW_INVALID_CROP;
    }
    // An active/unavailable timing owner must be rejected without changing
    // mode, callbacks or shared scratch (including recovery attempts).
    if (!epd_lcd_set_line_timing(0, 0)) return EPD_DRAW_NO_PHASES_AVAILABLE;
    const EpdRect saved_area = ctx->area;
    ctx->error = EPD_DRAW_SUCCESS;
    ctx->static_line_buffer = ctx->feed_line_buffers[0];
    epd_set_mode(1);
    // Full HOLD pass establishes a neutral source latch before row prefixes.
    if (!ctx->error) (void)timed_full_hold(ctx);
    for (int i = 14; i >= 0 && !ctx->error; --i) {
        int64_t dark_at = 0, neutral_at = 0;
        if (!timed_scan(ctx, cells[i], true, 0, &dark_at)) break;
        // Matching row windows: compare actual DATA starts, not API call time.
        if (!timed_scan(ctx, cells[i], false, dark_at + sample_durations_us[i], &neutral_at)) break;
        ESP_LOGI("EpdTiming", "sample code=%d requested=%dus actual scan-to-HOLD=%lldus first=%d rows=%d",
            i, sample_durations_us[i], (long long)(neutral_at-dark_at), cells[i].y, cells[i].height);
    }
    // On failure attempt a bounded HOLD pass; a broken bus cannot guarantee
    // electrical neutralization. Caller also marks optical state unknown.
    if (ctx->error) {
        const enum EpdDrawError error = ctx->error;
        ctx->error = EPD_DRAW_SUCCESS;
        // Do NOT prefix-skip with a possibly dark latch after a broken scan.
        // A full neutral sweep starts at row zero and establishes HOLD again.
        (void)timed_full_hold(ctx);
        ctx->error |= error;
    }
    epd_lcd_line_source_cb(NULL,NULL);
    epd_lcd_frame_done_cb(NULL,NULL);
    epd_set_mode(0);
    const bool restored_window = epd_lcd_set_scan_window(0,0);
    const bool restored_timing = epd_lcd_set_line_timing(0,0);
    if (!restored_window || !restored_timing)
        ctx->error |= EPD_DRAW_NO_PHASES_AVAILABLE;
    ctx->static_line_buffer = NULL;
    ctx->area = saved_area;
    return ctx->error;
}
#endif
