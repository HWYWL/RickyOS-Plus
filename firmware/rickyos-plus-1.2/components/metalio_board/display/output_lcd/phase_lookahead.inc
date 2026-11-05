// Included only by render_lcd.c when explicitly testing phase lookahead.
// A phase's LUT is reused ONLY after both feeders acknowledge completion.
// The ISR consumes committed internal queue rows, never the LUT or PSRAM.
// Opt-in optical A/B: launch intervals, not CKV/line timing or LUT counts.
#ifndef RICKYOS_PLUS_READER_MOTION_PERIOD_US
#define RICKYOS_PLUS_READER_MOTION_PERIOD_US 0
#endif
#if RICKYOS_PLUS_READER_MOTION_PERIOD_US != 0 && \
    (RICKYOS_PLUS_READER_MOTION_PERIOD_US < EPD_MIN_PHASE_US || RICKYOS_PLUS_READER_MOTION_PERIOD_US > 25000)
#error "Reader motion period must be 0 or 12000..25000 us"
#endif
static int reader_motion_period_us(const RenderContext_t* ctx) {
#if defined(CONFIG_IDF_TARGET_ESP32S31) && RICKYOS_PLUS_READER_MOTION_PERIOD_US
    if (ctx->turn_phases && !ctx->turn_region.spread && ctx->reader_text_finish &&
        ctx->cycle_frames > ctx->turn_phases) return RICKYOS_PLUS_READER_MOTION_PERIOD_US;
#else
    (void)ctx;
#endif
    return 0;
}

static bool wait_phase_feeders(RenderContext_t* ctx, unsigned* pending) {
    for (int i = 0; i < NUM_RENDER_THREADS; ++i) {
        if (!(*pending & (1u << i))) continue;
        if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
            lcd_note_first_fault(&s_first_fault, LCD_FAULT_FEED_TIMEOUT, -1, i);
            __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
            return false;
        }
        *pending &= ~(1u << i);
    }
    return true;
}

static void start_phase_feeders(RenderContext_t* ctx, unsigned* pending) {
    *pending = (1u << NUM_RENDER_THREADS) - 1;
    for (int i = 0; i < NUM_RENDER_THREADS; ++i) xTaskNotifyGive(ctx->feed_tasks[i]);
}

// Opt-in A/B: start the next phase's producers only after the current frame has
// been scanned, with a smaller prefill. Measured ~2 ms slower per phase and it did
// not remove the late bounce refills near row 552 (those came from the EOF ISR
// waiting out cache-disabled flash access; see CONFIG_LCD_RGB_ISR_IRAM_SAFE).
#ifndef RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME
#define RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME 0
#endif
#define LOOKAHEAD_AFTER_FRAME_PREFILL 96

static int lookahead_prefill_lines(void) {
    return RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME ? LOOKAHEAD_AFTER_FRAME_PREFILL : s_prefill_lines;
}

static bool wait_phase_prefill(RenderContext_t* ctx) {
    const int64_t deadline = esp_timer_get_time() + 2000000;
    for (int row = 0; row < lookahead_prefill_lines(); ++row) {
        while (!__atomic_load_n(&ctx->line_ready[row], __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE) || esp_timer_get_time() >= deadline) {
                if (!__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE))
                    lcd_note_first_fault(&s_first_fault, LCD_FAULT_PREFILL_TIMEOUT, row, -1);
                __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
                return false;
            }
            vTaskDelay(0);
        }
    }
    return true;
}

static void lcd_do_update_lookahead(RenderContext_t* ctx) {
    const int64_t cycle_start = esp_timer_get_time();
    const int motion_period = reader_motion_period_us(ctx);
    uint8_t* const original_ready = ctx->line_ready;
    uint8_t* const original_threads = ctx->line_threads;
    unsigned pending = 0;
    int periods = 0, period_min = INT32_MAX, period_max = 0;
    int64_t previous_start = 0, period_total = 0;
    prepare_context_for_next_frame(ctx);
    if (!epd_lcd_set_line_timing(0, 0)) {
        ctx->error |= EPD_DRAW_NO_PHASES_AVAILABLE;
        return;
    }
    s_phase_producer_ready = original_ready;
    __atomic_store_n(&s_phase_lookahead, true, __ATOMIC_RELEASE);
    epd_set_mode(1);
    start_phase_feeders(ctx, &pending);

    for (int phase = 0; phase < ctx->cycle_frames; ++phase) {
        // Previous EOF detached the callbacks; neither ISR nor a producer still
        // uses this old ready pointer. Only the consumer fields change here.
        ctx->line_ready = s_phase_producer_ready;
        ctx->lines_consumed = 0;
        if (!wait_phase_prefill(ctx)) break;
        if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) break;
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        epd_lcd_line_source_cb((line_cb_func_t)retrieve_line_isr, ctx);
        // Anchor to the preceding ACTUAL launch. A late phase never causes
        // a shortened catch-up interval. Prefill is still mandatory first;
        // the hardware waits outside its critical section for this deadline.
        const int64_t deadline = motion_period && previous_start ? previous_start + motion_period : 0;
        const int64_t started = epd_lcd_start_frame_at(deadline);
        if (previous_start) {
            const int period = (int)(started - previous_start);
            if (period < period_min) period_min = period;
            if (period > period_max) period_max = period;
            period_total += period;
            ++periods;
        }
        previous_start = started;

        // Current feeders finish while the ISR is still draining their queue
        // tail. Append the next phase behind that tail; ownership is row%2.
        if (!wait_phase_feeders(ctx, &pending)) break;
        if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) break;
#if !RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME
        if (phase + 1 < ctx->cycle_frames) {
            uint8_t* next = ctx->line_ready == original_ready ? original_threads : original_ready;
            memset(next, 0, ctx->lines_total);
            const EpdWaveformPhases* phases =
                ctx->waveform->mode_data[ctx->waveform_index]->range_data[ctx->waveform_range];
            if (ctx->turn_phases) {
                // Current ISR consumes only committed internal rows. Both
                // producers have joined, so changing their phase/window is safe.
                ctx->turn_producer_tick = phase + 1;
                ricky_turn_prepare_window(ctx);
            } else {
                ctx->lut_build_func(ctx->conversion_lut, phases, phase + 1);
            }
            s_phase_producer_ready = next;
            start_phase_feeders(ctx, &pending);
        }
#endif
        if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            lcd_note_first_fault(&s_first_fault, LCD_FAULT_FRAME_TIMEOUT, -1, -1);
            __atomic_fetch_or(&ctx->error, EPD_DRAW_EMPTY_LINE_QUEUE, __ATOMIC_RELEASE);
            break;
        }
        if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) break;
#if RICKYOS_PLUS_LOOKAHEAD_AFTER_FRAME
        // The frame is fully scanned: the ISR no longer reads either queue.
        if (phase + 1 < ctx->cycle_frames) {
            uint8_t* next = ctx->line_ready == original_ready ? original_threads : original_ready;
            memset(next, 0, ctx->lines_total);
            const EpdWaveformPhases* phases =
                ctx->waveform->mode_data[ctx->waveform_index]->range_data[ctx->waveform_range];
            if (ctx->turn_phases) {
                // Current ISR consumes only committed internal rows. Both
                // producers have joined, so changing their phase/window is safe.
                ctx->turn_producer_tick = phase + 1;
                ricky_turn_prepare_window(ctx);
            } else {
                ctx->lut_build_func(ctx->conversion_lut, phases, phase + 1);
            }
            s_phase_producer_ready = next;
            start_phase_feeders(ctx, &pending);
        }
#endif
        // Keep the existing minimum settling cadence, timed from actual scan
        // start (not from before LUT build/prefill). No shortened waveforms.
        // Only the final phase needs its full hold here. Other fixed-cadence
        // phases wait in the next hardware start, including its setup/prefix,
        // rather than adding that setup time after a full software delay.
        if (motion_period && phase + 1 == ctx->cycle_frames)
            pace_phase_min(started, motion_period);
        else
            pace_phase(started);
        ++ctx->current_frame;
    }

    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
        epd_lcd_line_source_cb(NULL, NULL);
        epd_lcd_abort_frame();
        // Producers see error each row / queue wait. Never restore/reuse a LUT
        // or ready map while a stale producer could still write to it.
        if (!wait_phase_feeders(ctx, &pending)) {
            ESP_LOGE("epd_lcd", "feeder failed to quiesce; cannot reuse render context");
            abort();  // Fail closed on a stuck task, rather than corrupt the next frame.
        }
        xSemaphoreTake(ctx->frame_done, 0);
        for (int i = 0; i < NUM_RENDER_THREADS; ++i) lq_reset(&ctx->line_queues[i]);
    }
    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);
    __atomic_store_n(&s_phase_lookahead, false, __ATOMIC_RELEASE);
    s_phase_producer_ready = NULL;
    ctx->line_ready = original_ready;
    ctx->line_threads = original_threads;
    epd_set_mode(0);
    if (__atomic_load_n(&ctx->error, __ATOMIC_ACQUIRE)) {
        ESP_LOGE("epd_lcd", "first fault since boot=0x%08lx native=%d producer_tick=%d current=%d",
                 (unsigned long)epd_lcd_get_first_fault(), ctx->turn_phases,
                 ctx->turn_producer_tick, ctx->current_frame);
    }
    if (!epd_lcd_set_line_timing(0, 0)) ctx->error |= EPD_DRAW_NO_PHASES_AVAILABLE;
    if (periods) {
        __atomic_store_n(&last_phase_average, (int)(period_total / periods), __ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_minimum, period_min, __ATOMIC_RELAXED);
        __atomic_store_n(&last_phase_maximum, period_max, __ATOMIC_RELAXED);
        ESP_LOGI("EpdTiming", "lookahead cadence us: avg=%d min=%d max=%d frames=%d cycle=%dms",
                 (int)(period_total / periods), period_min, period_max, ctx->cycle_frames,
                 (int)((esp_timer_get_time() - cycle_start) / 1000));
    }
}
