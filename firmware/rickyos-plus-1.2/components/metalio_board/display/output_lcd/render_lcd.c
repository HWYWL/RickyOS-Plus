#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/cache.h>

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "../output_common/ricky_turn_line.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"
#include "lcd_fault.h"

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

// Same proven prefill as the legacy path. Never reduce it for the experiment.
static int s_prefill_lines = 192;
static uint32_t s_first_fault;
uint32_t epd_lcd_get_first_fault(void) {
    return __atomic_load_n(&s_first_fault, __ATOMIC_ACQUIRE);
}
#if RICKYOS_PLUS_PHASE_LOOKAHEAD
// The driver already owns one serialized RenderContext. Reuse its two internal
// per-row arrays as alternating ready maps; no extra queues, frames or tasks.
static bool s_phase_lookahead;
static uint8_t* s_phase_producer_ready;
#endif

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread =
#if RICKYOS_PLUS_PHASE_LOOKAHEAD
        __atomic_load_n(&s_phase_lookahead, __ATOMIC_ACQUIRE)
            ? ctx->lines_consumed % NUM_RENDER_THREADS :
#endif
        ctx->line_threads[ctx->lines_consumed];
    BaseType_t awoken = pdFALSE;

    if (!__atomic_load_n(&ctx->line_ready[ctx->lines_consumed],__ATOMIC_ACQUIRE) ||
        thread >= NUM_RENDER_THREADS) {
        lcd_note_first_fault(&s_first_fault, LCD_FAULT_ROW_NOT_READY, ctx->lines_consumed, thread);
        __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return awoken;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    if (lq_read(lq, buf) != 0) {
        lcd_note_first_fault(&s_first_fault, LCD_FAULT_QUEUE_EMPTY, ctx->lines_consumed, thread);
        __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    // 扫描先于供数结束时置错，禁止残留行进入下一相位
    if (ctx->lines_consumed < ctx->lines_total) {
        lcd_note_first_fault(&s_first_fault, LCD_FAULT_FRAME_SHORT, ctx->lines_consumed, -1);
        __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
    }

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

// 纯扫约 11ms；TB8 相太短 DU/GL 易残影。流式路径保留，仅垫相周期下限。
#ifndef EPD_MIN_PHASE_US
#define EPD_MIN_PHASE_US 12000
#endif

static int last_phase_average, last_phase_minimum, last_phase_maximum;
void epd_lcd_get_timing(int* average,int* minimum,int* maximum) {
    *average=__atomic_load_n(&last_phase_average,__ATOMIC_RELAXED);
    *minimum=__atomic_load_n(&last_phase_minimum,__ATOMIC_RELAXED);
    *maximum=__atomic_load_n(&last_phase_maximum,__ATOMIC_RELAXED);
}

static void pace_phase_min(int64_t phase_start_us, int minimum_us) {
    for (;;) {
        if (esp_timer_get_time() - phase_start_us >= minimum_us) {
            break;
        }
        vTaskDelay(0);
    }
}

static void pace_phase(int64_t phase_start_us) {
    pace_phase_min(phase_start_us, EPD_MIN_PHASE_US);
}

#if RICKYOS_PLUS_PHASE_LOOKAHEAD
#include "phase_lookahead.inc"
#endif

void lcd_do_update(RenderContext_t* ctx) {
#if RICKYOS_PLUS_PHASE_LOOKAHEAD
    // Text turns (GL16_FAST) and UI area updates (GL16) share the scheduling; it only
    // overlaps preparing the next phase with scanning this one, never the waveform.
    const int lookaheadMode = ctx->mode & 0x3f;
    if ((lookaheadMode == MODE_GL16_FAST || lookaheadMode == MODE_GL16 || lookaheadMode == MODE_GL16_INV ||
         lookaheadMode == MODE_GC16_FAST) &&
        (ctx->mode & MODE_PACKING_1PPB_DIFFERENCE)
        && ctx->phase_times == NULL && ctx->cycle_frames > 1 && ctx->lines_total > s_prefill_lines
        && ctx->line_threads && ctx->line_ready) {
        lcd_do_update_lookahead(ctx);
        return;
    }
#endif
    const int64_t cycle_start=esp_timer_get_time();
    int64_t previous_phase_start=0, period_total=0;
    int period_min=INT32_MAX, period_max=0, periods=0;
    epd_set_mode(1);
    ESP_LOGD("epd_lcd", "update %d frames, lines=%d (stream pace=%dus)", ctx->cycle_frames,
             ctx->lines_total, EPD_MIN_PHASE_US);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        const int64_t phase_start=esp_timer_get_time();
        if (previous_phase_start) {
            const int period=(int)(phase_start-previous_phase_start);
            if (period<period_min) period_min=period;
            if (period>period_max) period_max=period;
            period_total+=period; ++periods;
        }
        previous_phase_start=phase_start;
        prepare_context_for_next_frame(ctx);
        // NULL timing keeps the proven board profile, not the I2S 12us
        // default. Explicit LCD timing is a complete line period in 0.1us.
        if (!epd_lcd_set_line_timing(ctx->phase_times ? ctx->frame_time : 0, 0)) {
            ctx->error |= EPD_DRAW_NO_PHASES_AVAILABLE;
            break;
        }
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);

        const int64_t t0 = esp_timer_get_time();

        // start both feeder tasks；开扫由 feeder 预填够行后触发
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);
        unsigned pending = (1u << NUM_RENDER_THREADS) - 1;

        if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            lcd_note_first_fault(&s_first_fault, LCD_FAULT_FRAME_TIMEOUT, -1, -1);
            ESP_LOGE("epd_lcd", "LCD frame %d/%d timeout", k + 1, ctx->cycle_frames);
            __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_abort_frame();
            xSemaphoreTake(ctx->frame_done, 0);
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
                lcd_note_first_fault(&s_first_fault, LCD_FAULT_FEED_TIMEOUT, -1, i);
                ESP_LOGE("epd_lcd", "feed %d timeout @frame %d", i, k);
                __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
            } else {
                pending &= ~(1u << i);
            }
        }

        pace_phase(phase_start);

        const int64_t period_us = esp_timer_get_time() - t0;
        if ((k & 3) == 0 || k + 1 == ctx->cycle_frames) {
            ESP_LOGD("epd_lcd", "phase %d/%d period=%lldus", k + 1, ctx->cycle_frames,
                     (long long)period_us);
        }

        if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_abort_frame();
            // A timed-out take did not consume the worker's eventual ack.
            // Stop DMA first, then join every pending producer before resetting
            // queues or returning a reusable context (including to standby).
            // Match the lookahead path's existing fail-closed contract if a
            // worker cannot quiesce; returning would expose stale writes.
            for (int i = 0; i < NUM_RENDER_THREADS; ++i) {
                if ((pending & (1u << i)) &&
                    xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
                    ESP_LOGE("epd_lcd", "feed %d failed to quiesce; cannot reuse render context", i);
                    abort();
                }
            }
            xSemaphoreTake(ctx->frame_done, 0);
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ctx->current_frame++;
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
        ESP_LOGE("epd_lcd", "first fault since boot=0x%08lx", (unsigned long)epd_lcd_get_first_fault());
    }
    // Restore after success, timeout or rejected timing; tests cannot leak
    // their gate/line setting into a later UI, clear or reading update.
    if (!epd_lcd_set_line_timing(0, 0)) ctx->error |= EPD_DRAW_NO_PHASES_AVAILABLE;
    if (periods) {
        __atomic_store_n(&last_phase_average,(int)(period_total/periods),__ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_minimum,period_min,__ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_maximum,period_max,__ATOMIC_RELAXED);
        ESP_LOGI("EpdTiming","phase cadence us: avg=%d min=%d max=%d frames=%d cycle=%dms",
        (int)(period_total/periods),period_min,period_max,ctx->cycle_frames,
        (int)((esp_timer_get_time()-cycle_start)/1000));
    }
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
push_pixels_isr(RenderContext_t* ctx, uint8_t* buf) {
    // Output no-op outside of drawn area
    if (ctx->lines_consumed < ctx->area.y) {
        memset(buf, 0, ctx->display_width / 4);
    } else if (ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        memset(buf, 0, ctx->display_width / 4);
    } else {
        memcpy(buf, ctx->static_line_buffer, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return pdFALSE;
}

/**
 * Populate the line mask for use in epd_push_pixels.
 */
static void push_pixels_populate_line(RenderContext_t* ctx, int color) {
    // Select fill pattern by draw color
    int fill_byte = 0;
    switch (color) {
        case 0:
            fill_byte = DARK_BYTE;
            break;
        case 1:
            fill_byte = CLEAR_BYTE;
            break;
        default:
            fill_byte = 0x00;
    }

    // Compute a line mask based on the drawn area
    uint8_t* dirtyness = malloc(ctx->display_width / 2);
    assert(dirtyness != NULL);

    memset(dirtyness, 0, ctx->display_width / 2);

    for (int i = 0; i < ctx->display_width; i++) {
        if ((i >= ctx->area.x) && (i < ctx->area.x + ctx->area.width)) {
            dirtyness[i / 2] |= i % 2 ? 0xF0 : 0x0F;
        }
    }
    epd_populate_line_mask(ctx->line_mask, dirtyness, ctx->display_width / 4);

    // mask the line pattern with the populated mask
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);

    free(dirtyness);
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    ctx->static_line_buffer = malloc(ctx->display_width / 4);
    assert(ctx->static_line_buffer != NULL);

    push_pixels_populate_line(ctx, color);
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        lcd_note_first_fault(&s_first_fault, LCD_FAULT_FRAME_TIMEOUT, -1, -1);
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        epd_lcd_line_source_cb(NULL, NULL);
        epd_lcd_frame_done_cb(NULL, NULL);
        epd_lcd_abort_frame();
        ESP_LOGE("epd_lcd", "clear scan timed out");
    }
    epd_set_mode(0);

    free(ctx->static_line_buffer);
    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

// 预填够 bounce 再开扫；太少易欠载，太多则短队列在开扫前会堵死
// S31 uses C LUT feeding instead of Pico's Xtensa SIMD. The 64-line profile
// underruns during concurrent audio. Alternate producers to prefill 192 rows
// without exceeding either existing 127-slot queue.

__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;
#if RICKYOS_PLUS_PHASE_LOOKAHEAD
    const bool lookahead = __atomic_load_n(&s_phase_lookahead, __ATOMIC_ACQUIRE);
    uint8_t* ready = lookahead ? s_phase_producer_ready : ctx->line_ready;
#else
    const bool lookahead = false;
    uint8_t* ready = ctx->line_ready;
#endif

    // 另一生产者可能已启动并报告欠载；不得再次启动同一帧
    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
        return;
    }

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) return;
    assert(area.width == ctx->display_width && area.x == 0);

    const int trigger_line = int_min(s_prefill_lines, ctx->lines_total-1);

    // Fixed ownership bounds each producer to half the prefill. With dynamic
    // claims one core can fill its 127-slot queue before a larger prefill starts.
    for (l=thread_id; l<ctx->lines_total; l+=NUM_RENDER_THREADS) {
        if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) return;
        if (!lookahead) ctx->line_threads[l] = (uint8_t)thread_id;

        // 预填够行后开扫，随后边扫边喂
        if (!lookahead && l == trigger_line) {
            // Claimed rows are not necessarily committed: the other core may
            // still own an early row. Start scanning only when the entire
            // prefill prefix is ready, not when a producer claims row 127.
            const int64_t ready_deadline=esp_timer_get_time()+2000000;
            for (int row=0;row<l;++row) {
                while (!__atomic_load_n(&ctx->line_ready[row],__ATOMIC_ACQUIRE)) {
                    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE) || esp_timer_get_time()>ready_deadline) {
                        if (!__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE))
                            lcd_note_first_fault(&s_first_fault, LCD_FAULT_PREFILL_TIMEOUT, row, thread_id);
                        __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE); return;
                    }
                    vTaskDelay(0);
                }
            }
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            int wait_iters = 0;
            while (buf == NULL) {
                if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
                    return;
                }
                buf = lq_current(lq);
                if (buf != NULL) {
                    break;
                }
                // A prefilled next phase can fill both rings after the current
                // scan ends. Yield a tick so the lower-priority controller can
                // start it; spinning at feeder priority would deadlock it.
                vTaskDelay(lookahead ? 1 : 0);
                if (++wait_iters > 30000) {
                    lcd_note_first_fault(&s_first_fault, LCD_FAULT_QUEUE_TIMEOUT, l, thread_id);
                    __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
                    return;
                }
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            __atomic_store_n(&ready[l],1,__ATOMIC_RELEASE);
            continue;
        }

        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);
#if !CONFIG_IDF_TARGET_ESP32S31
        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);
#endif
        uint32_t* lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        int wait_iters = 0;
        while (buf == NULL) {
            if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
                return;
            }
            buf = lq_current(lq);
            if (buf != NULL) {
                break;
            }
            vTaskDelay(lookahead ? 1 : 0);
            if (++wait_iters > 30000) {
                lcd_note_first_fault(&s_first_fault, LCD_FAULT_QUEUE_TIMEOUT, l, thread_id);
                __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
                return;
            }
        }

        if (ctx->turn_phases) ricky_turn_line(ctx, lp, buf, l);
        else ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);
        lq_commit(lq);
        __atomic_store_n(&ready[l],1,__ATOMIC_RELEASE);
    }
}

#endif
