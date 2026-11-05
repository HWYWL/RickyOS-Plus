#pragma once

#include <driver/gpio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * LCD bus configuration parameters.
 */
typedef struct {
    // GPIO numbers of the parallel bus pins.
    gpio_num_t data[16];

    // horizontal clock pin.
    gpio_num_t clock;
    // vertical clock pin
    gpio_num_t ckv;

    // horizontal "Start Pulse", enabling data input on the line shift register
    gpio_num_t start_pulse;
    // latch enable
    gpio_num_t leh;
    // vertical start pulse, resetting the vertical line shift register.
    gpio_num_t stv;
} lcd_bus_config_t;

/// Configuration structure for the LCD-based Epd driver.
typedef struct {
    // high time for CKV in 1/10us.
    size_t pixel_clock;    // = 12000000
    int ckv_high_time;     // = 70
    int line_front_porch;  // = 4
    int le_high_time;      // = 4
    int bus_width;         // = 16
    lcd_bus_config_t bus;
    int dummy_clocks_after_data; // Minimum XCL clocks before the next XLE pulse.
    int white_border_pixels; // Multiple of four; output may only hold/whiten here.
} LcdEpdConfig_t;

typedef bool (*line_cb_func_t)(void*, uint8_t*);
typedef void (*frame_done_func_t)(void*);

void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height);
// Task-only, with no active renderer. Failed setup releases partial resources
// and returns false, allowing USB/logging to survive a sleep restore failure.
bool epd_lcd_init_checked(const LcdEpdConfig_t* config, int display_width, int display_height);
void epd_lcd_deinit();
void epd_lcd_frame_done_cb(frame_done_func_t, void* payload);
void epd_lcd_line_source_cb(line_cb_func_t, void* payload);
void epd_lcd_start_frame();
void epd_lcd_prime_white_border(bool enable);
int epd_lcd_white_border_pixels(void);
/**
 * @brief 中止当前帧（停 LCD/GDMA），供超时路径使用
 */
void epd_lcd_abort_frame();
/**
 * Set the LCD pixel clock frequency in MHz.
 */
void epd_lcd_set_pixel_clock_MHz(int frequency);
/** Task-only, between scans. Units 0.1us; zero restores board defaults.
 * Rejects live changes, unsafe data/CKV windows and unsupported periods.
 * Gate width is NOT the electrophoretic exposure time of a pixel.
 */
bool epd_lcd_set_line_timing(int period_us10, int gate_high_us10);
/** Data-scan deadline; zero starts now. Returns measured LCD start timestamp. */
int64_t epd_lcd_start_frame_at(int64_t scan_at_us);
#if RICKYOS_PLUS_GRAY_CALIBRATION
/** Diagnostic only. Requires source HOLD before prefix; (0,0) restores full scan. */
bool epd_lcd_set_scan_window(int first, int padded_lines);
#endif
