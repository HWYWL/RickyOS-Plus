#include "plus_i2c_wire.h"
/**
 * @file epd_board_metalio_eink4_plus.c
 * @brief Metalio ESP32-S31 4.7" 并行墨水屏板级（E0470A14-AF 1216x684）
 * @note 引脚来自板级 config.h / 原理图 V1.5；TPS 控制脚经 TCA9555
 */

#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"

#include "lcd_driver.h"
#include "metalio_sd.h"
#include "pca9555.h"
#include "metalio_i2c1.h"
#include "tps65185.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <hal/i2c_ll.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#define TAG "epd_eink4p"

/* I2C：TPS/TCA 同总线；TCA 开漏 INT → ESP */
#define CFG_SCL I2C_SCL_PIN
#define CFG_SDA I2C_SDA_PIN
#define CFG_INTR IO_EXPANDER_INT_GPIO
#define EPDIY_I2C_PORT I2C_NUM_1

/* TCA 位/口：一律由 config.h 的 EPD_TPS_IO_* / EPD_IO_* 推出 */
#define CFG_IO_BIT(io)  (1u << ((unsigned)(io) & 7u))
#define CFG_IO_PORT(io) ((int)((unsigned)(io) >> 3))

#define CFG_PIN_VCOM_CTRL CFG_IO_BIT(EPD_TPS_IO_VCOM_CTRL)
#define CFG_PIN_PWRUP     CFG_IO_BIT(EPD_TPS_IO_PWRUP)
#define CFG_PIN_WAKEUP    CFG_IO_BIT(EPD_TPS_IO_WAKEUP)
#define CFG_PIN_PWRGOOD   CFG_IO_BIT(EPD_TPS_IO_PWR_GOOD)
#define CFG_PIN_INT       CFG_IO_BIT(EPD_TPS_IO_INT)
#define CFG_PIN_MOTOR     CFG_IO_BIT(EPD_IO_MOTOR)
#define CFG_PIN_PA_EN     CFG_IO_BIT(EPD_IO_PA_EN)
#define CFG_PIN_CAM_SCR   CFG_IO_BIT(EPD_IO_CAM_SCR_EN)
#define CFG_PIN_TP_RST    CFG_IO_BIT(EPD_IO_TP_RST)
#define CFG_PIN_BT_PA_PWR CFG_IO_BIT(EPD_IO_BT_PA_PWR)
#define CFG_PIN_PWR_PULSE CFG_IO_BIT(EPD_IO_PWR_PULSE)

#define CFG_PORT_CTRL CFG_IO_PORT(EPD_TPS_IO_VCOM_CTRL) // Port0：VCOM/PWRUP/WAKEUP/PWR_GOOD
#define CFG_PORT_INT  CFG_IO_PORT(EPD_TPS_IO_INT)       // Port1：TPS_INT / TP_RST / 关机脉冲

/* Port0：TPS/Motor/PA/摄像屏电 输出；PWR_GOOD 等为输入 */
#define CFG_P0_TPS_OUT_MASK (CFG_PIN_VCOM_CTRL | CFG_PIN_PWRUP | CFG_PIN_WAKEUP)
#define CFG_P0_OUTPUT_MASK                                                                 \
    (CFG_P0_TPS_OUT_MASK | CFG_PIN_MOTOR | CFG_PIN_PA_EN | CFG_PIN_CAM_SCR)
#define CFG_P0_INPUT_MASK ((uint8_t)(~CFG_P0_OUTPUT_MASK))

/* Port1：TP_RST/P14/关机脉冲为输出；P16=RTC_INT 等为输入 */
#define CFG_P1_RAIL_MASK   (CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR)
#define CFG_P1_OUTPUT_MASK (CFG_P1_RAIL_MASK | CFG_PIN_PWR_PULSE)
#define CFG_P1_INPUT_MASK  ((uint8_t)(~CFG_P1_OUTPUT_MASK))
// Inputs left armed in standby light sleep: the power key only, as the OEM does.
// (park6 also armed the side keys; with P14 off the Plus then powered itself off.)
#define CFG_P1_SLEEP_WAKE_KEYS ((uint8_t)CFG_IO_BIT(EPD_IO_POWER))

/* 并行数据 / 控制：板级 config.h */
#define D0 EPD_PIN_D0
#define D1 EPD_PIN_D1
#define D2 EPD_PIN_D2
#define D3 EPD_PIN_D3
#define D4 EPD_PIN_D4
#define D5 EPD_PIN_D5
#define D6 EPD_PIN_D6
#define D7 EPD_PIN_D7

#define CKH EPD_PIN_XCL
#define LEH EPD_PIN_XLE
#define EPD_OE EPD_PIN_XOE
#define STH EPD_PIN_XSTL
#define STV EPD_PIN_SPV
#define EPD_MODE EPD_PIN_MODE
#define EPD_BORDER EPD_PIN_BORDER
#define CKV EPD_PIN_CKV

typedef struct {
    i2c_port_t port;
    bool pwrup;
    bool vcom_ctrl;
    bool wakeup;
} epd_config_register_t;

static int vcom = EPD_VCOM_MV;
static epd_config_register_t config_reg;
static bool interrupt_done = false;
static SemaphoreHandle_t s_ioexp_irq_sem = NULL;
static TaskHandle_t s_gpio2_irq_task = NULL;

static void IRAM_ATTR interrupt_handler(void* arg) {
    (void)arg;
    interrupt_done = true;
    BaseType_t hp = pdFALSE;
    if (s_ioexp_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_ioexp_irq_sem, &hp);
    }
    if (s_gpio2_irq_task != NULL) {
        vTaskNotifyGiveFromISR(s_gpio2_irq_task, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static lcd_bus_config_t lcd_config = {
    .clock = CKH,
    .ckv = CKV,
    .leh = LEH,
    .start_pulse = STH,
    .stv = STV,
    .data[0] = D0,
    .data[1] = D1,
    .data[2] = D2,
    .data[3] = D3,
    .data[4] = D4,
    .data[5] = D5,
    .data[6] = D6,
    .data[7] = D7,
    .data[8] = D7,
    .data[9] = D7,
    .data[10] = D7,
    .data[11] = D7,
    .data[12] = D7,
    .data[13] = D7,
    .data[14] = D7,
    .data[15] = D7,
};

static bool s_ioexp_ok = false;
static bool s_tps_ok = false;
static void epd_board_poweroff(epd_ctrl_state_t* state);
static bool s_lcd_bus_held = false;       // 入睡已拆 LCD 并置低总线脚
// Port1 锁存：P12/P14 默认高（上电），关机脉冲默认低
static uint8_t s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
static bool s_sleep_keys_restored;
static uint8_t s_motor_on = 0;             // Port0 马达位锁存，0 或 CFG_PIN_MOTOR
static uint8_t s_pa_on = 0;                // Port0 PA_EN 锁存，0 或 CFG_PIN_PA_EN
static uint8_t s_cam_scr_on = CFG_PIN_CAM_SCR; // Port0 P05 摄像/屏电，默认高

static uint8_t p0_output_value(void) {
    uint8_t value = 0x00;
    if (config_reg.pwrup) {
        value |= CFG_PIN_PWRUP;
    }
    if (config_reg.vcom_ctrl) {
        value |= CFG_PIN_VCOM_CTRL;
    }
    if (config_reg.wakeup) {
        value |= CFG_PIN_WAKEUP;
    }
    value |= s_motor_on;
    value |= s_pa_on;
    value |= s_cam_scr_on;
    return value;
}

static void p0_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p0_write: %s", esp_err_to_name(err));
    }
}

static void p1_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p1_write: %s", esp_err_to_name(err));
    }
}

bool epd_board_metalio_eink4_plus_tps_ok(void) {
    return s_tps_ok;
}

bool epd_board_metalio_eink4_plus_ioexp_ok(void) {
    return s_ioexp_ok;
}

void epd_board_metalio_eink4_plus_motor_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    pca9555_set_motor(on);
}

void epd_board_metalio_eink4_plus_pa_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    s_pa_on = on ? CFG_PIN_PA_EN : 0;
    p0_write();
}

void epd_board_metalio_eink4_plus_ana_sw_set(bool high) {
    static bool s_gpio_ready = false;
    if (!s_gpio_ready) {
        // Preserve the existing USB route while taking ownership: reset_pin
        // releases the pad and a zero output latch can briefly select MSC.
        gpio_set_level(ANA_SW_GPIO, high ? 1 : 0);
        gpio_set_direction(ANA_SW_GPIO, GPIO_MODE_OUTPUT);
        gpio_pullup_dis(ANA_SW_GPIO);
        gpio_pulldown_dis(ANA_SW_GPIO);
        s_gpio_ready = true;
    }
    gpio_set_level(ANA_SW_GPIO, high ? 1 : 0);
}

// Standby (lazy BT/PA): P14 feeds the BH1098 Bluetooth/audio module and the
// NS4150 amp, ~8 mA idle. Standby turns it off and leaves it off on wake;
// the next audio session turns it back on. Re-enabling it in the wake path
// reset the board with a power glitch (park1), so it is never done there.
static bool s_bt_pa_parked = false;

void epd_board_metalio_eink4_plus_sleep_park_bt_pa(void) {
    if (!s_ioexp_ok) {
        return;
    }
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    p1_write();
    s_bt_pa_parked = true;
    ESP_LOGI(TAG, "standby: P14 (BT/PA) off until audio is needed");
}

bool epd_board_metalio_eink4_plus_bt_pa_resume(void) {
    if (!s_ioexp_ok || !s_bt_pa_parked) {
        return false;
    }
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    p1_write();
    s_bt_pa_parked = false;
    ESP_LOGI(TAG, "audio: P14 (BT/PA) back on");
    return true;
}

// Standby (P5 only): the camera/screen rail is most of the parked current
// (~8 mA: park1 both rails cut ~2-3 mA, park5 P14 cut ~10 mA, park4 none ~10.5 mA).
// P14 stays on: with it off the panel did not change after wake (park5-8).
void epd_board_metalio_eink4_plus_sleep_cut_p5(void) {
    if (!s_ioexp_ok) {
        return;
    }
    s_cam_scr_on = 0;
    p0_write();
    ESP_LOGI(TAG, "standby: P5 (camera/screen) off");
}

bool epd_board_metalio_eink4_plus_sleep_restore_p5(void) {
    if (!s_ioexp_ok) {
        return false;
    }
    s_cam_scr_on = CFG_PIN_CAM_SCR;
    const bool ok = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(40));  // let the rail settle before the first refresh
    ESP_LOGI(TAG, "standby: P5 (camera/screen) back on ok=%d", ok);
    return ok;
}

void epd_board_metalio_eink4_plus_sleep_cut_rails(void) {
    s_sleep_keys_restored = false;
    if (!s_ioexp_ok) {
        return;
    }
    s_cam_scr_on = 0;
    p0_write();
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    p1_write();
    ESP_LOGI(TAG, "sleep cut P5 then P14");
}

void epd_board_metalio_eink4_plus_sleep_restore_rails(void) {
    if (!s_ioexp_ok || !s_sleep_keys_restored || !epd_board_metalio_eink4_plus_sleep_io_checked(false)) {
        s_sleep_keys_restored = false;
        return;
    }
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    p1_write();
    s_cam_scr_on = CFG_PIN_CAM_SCR;
    p0_write();
    ESP_LOGI(TAG, "sleep restore P14 then P5");
}

bool epd_board_metalio_eink4_plus_sleep_restore_rails_staged(bool keys_restored, void (*trace)(unsigned)) {
    // A successful function return was not proof of key restoration in static4:
    // its writes were unchecked. Require the checked result and a fresh state
    // read before tracing/issuing either rail write. No retry after a failure.
    if (!s_ioexp_ok || !s_sleep_keys_restored || !keys_restored || !epd_board_metalio_eink4_plus_sleep_io_checked(false)) {
        s_sleep_keys_restored = false;
        return false;
    }
    // Preserve static4 timing for this focused error-propagation change. The
    // 40 ms separation FAILED to prevent its reset; it is not a proven remedy.
    if (trace) trace(0);
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    bool ok = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT) == ESP_OK;
    if (trace) trace(1);
    vTaskDelay(pdMS_TO_TICKS(40));
    if (trace) trace(2);
    if (trace) trace(3);
    s_cam_scr_on = CFG_PIN_CAM_SCR;
    const esp_err_t p0_error = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (p0_error != ESP_OK) ok = false;
    if (trace) trace(4);
    vTaskDelay(pdMS_TO_TICKS(40));
    if (trace) trace(5);
    return ok;
}

bool epd_board_metalio_eink4_plus_sleep_io_checked(bool parked) {
    uint8_t regs[6] = {0}; // output[2], polarity[2], direction[2]
    if (!s_ioexp_ok || !metalio_i2c1_lock(20)) return false;
    // TCA9555 reads toggle only bit 0 of the register pointer (TI datasheet
    // 9.5.2.1.2), so a six-byte read at 0x02 repeats outputs three times.
    // Select each pair explicitly while retaining the one shared-bus lock.
    esp_err_t err = ESP_OK;
    for (unsigned pair = 0; pair < 3; ++pair) {
        err = plus_i2c_read(pca9555_get_addr(), (uint8_t)(0x02 + pair * 2), &regs[pair * 2], 2);
        if (err != ESP_OK) break;
    }
    metalio_i2c1_unlock();
    const uint8_t cfg0 = parked ? 0 : CFG_P0_INPUT_MASK;
    const uint8_t cfg1 = parked ? CFG_P1_SLEEP_WAKE_KEYS : CFG_P1_INPUT_MASK;
    return err == ESP_OK && regs[0] == p0_output_value() && regs[1] == s_p1_out &&
           regs[2] == 0 && regs[3] == 0 && regs[4] == cfg0 && regs[5] == cfg1;
}

bool epd_board_metalio_eink4_plus_ioexp_get_level(int io) {
    static uint8_t s_last[2] = {0xFF, 0xFF}; // 读失败保持；默认当高
    if (!s_ioexp_ok || io < 0 || io > 15) {
        return true;
    }
    const int port = CFG_IO_PORT(io);
    uint8_t v = s_last[port];
    if (pca9555_read_input_ex(config_reg.port, port, &v) == ESP_OK) {
        s_last[port] = v;
    }
    return (s_last[port] & CFG_IO_BIT(io)) != 0;
}

bool epd_board_metalio_eink4_plus_ioexp_read_port(int port, uint8_t* out) {
    if (!s_ioexp_ok || out == NULL || port < 0 || port > 1) {
        return false;
    }
    return pca9555_read_input_ex(config_reg.port, port, out) == ESP_OK;
}

bool epd_board_metalio_eink4_plus_ioexp_irq_wait(uint32_t timeout_ms) {
    if (s_ioexp_irq_sem == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_ioexp_irq_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void epd_board_metalio_eink4_plus_gpio2_irq_subscribe(TaskHandle_t task) {
    s_gpio2_irq_task = task;
}

void epd_board_metalio_eink4_plus_tps_enter_sleep(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // tip 的 poweroff 往往已把三脚拉低；SLEEP 过渡期勿再打 I2C，只补等待
    const bool already =
        !config_reg.vcom_ctrl && !config_reg.pwrup && !config_reg.wakeup;
    if (!already) {
        // 手册 8.4：PWRUP/VCOM↓ → WAKEUP↓ → 等 ≥100ms
        config_reg.vcom_ctrl = false;
        config_reg.pwrup = false;
        p0_write();
        vTaskDelay(pdMS_TO_TICKS(50));
        config_reg.wakeup = false;
        p0_write();
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "TPS SLEEP ready (already=%d)", already ? 1 : 0);
}

static const gpio_num_t k_epd_bus_pins[] = {
    D0, D1, D2, D3, D4, D5, D6, D7, CKH, LEH, EPD_OE, STH, STV, CKV, EPD_MODE, EPD_BORDER,
};

static void epd_ctrl_gpios_out_low(void) {
    gpio_reset_pin(EPD_OE);
    gpio_set_direction(EPD_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_OE, 0);
    gpio_reset_pin(EPD_MODE);
    gpio_set_direction(EPD_MODE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_MODE, 0);
    gpio_reset_pin(EPD_BORDER);
    gpio_set_direction(EPD_BORDER, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_BORDER, 0);
}

static bool epd_lcd_reinit(void) {
    const EpdDisplay_t* display = epd_get_display();
    LcdEpdConfig_t config = {
        .pixel_clock = display->bus_speed * 1000 * 1000,
        .ckv_high_time = 60,
        .line_front_porch = 4,
        .le_high_time = 9,
        .bus_width = display->bus_width,
        .bus = lcd_config,
        .dummy_clocks_after_data = 10, // E0470A14-AF section 6.3.1.
        .white_border_pixels = DISPLAY_WHITE_BORDER_PIXELS,
    };
    return epd_lcd_init_checked(&config, display->width, display->height);
}

void epd_board_metalio_eink4_plus_bus_hold_low(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    if (!s_lcd_bus_held) {
        epd_lcd_deinit();
        s_lcd_bus_held = true;
    }
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        const gpio_num_t pin = k_epd_bus_pins[i];
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        gpio_set_level(pin, 0);
    }
    ESP_LOGI(TAG, "EPD bus pins held low D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER");
}

void epd_board_metalio_eink4_plus_bus_latch_for_sleep(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        esp_err_t err = gpio_hold_en(k_epd_bus_pins[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_hold_en(%d): %s", (int)k_epd_bus_pins[i], esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "EPD bus pins latched for sleep");
}

bool epd_board_metalio_eink4_plus_bus_restore_checked(void) {
    if (!s_lcd_bus_held) return true;
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    epd_ctrl_gpios_out_low();
    if (!epd_lcd_reinit()) {
        s_tps_ok = false;
        ESP_LOGE(TAG, "EPD bus restore failed; keep display unavailable for retry");
        return false;
    }
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "EPD bus restored after sleep");
    return true;
}

void epd_board_metalio_eink4_plus_bus_restore(void) {
    (void)epd_board_metalio_eink4_plus_bus_restore_checked();
}

void epd_board_metalio_eink4_plus_boot_release_holds(void) {
    // 深睡 gpio_hold 跨 CPU 复位仍有效；不解则并行脚锁死为低，TPS 升压易失败
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    MetalioSd_ReleaseHold();
    gpio_hold_dis(CFG_SDA);
    gpio_hold_dis(CFG_SCL);
    gpio_hold_dis(FL_COOL_PIN);
    gpio_hold_dis(FL_WARM_PIN);
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "boot: released EPD/SD/I2C/FL gpio holds");
}

void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void) {
    s_sleep_keys_restored = false;
    if (!s_ioexp_ok) {
        return;
    }
    // Port0：TPS 三位与 PWR_GOOD 输出低；Motor/PA/P5 保持当前锁存
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;
    const uint8_t p0_out = p0_output_value();
    pca9555_set_value(config_reg.port, p0_out, CFG_PORT_CTRL);
    pca9555_set_config(config_reg.port, 0x00, CFG_PORT_CTRL);

    // Port1：P12/P14 锁存保留；音量/电源脚驱空闲高（勿驱低，醒后改输入会假出按下→松开）；P13 低
    const uint8_t rail = (uint8_t)(s_p1_out & CFG_P1_RAIL_MASK);
    const uint8_t keys_idle = (uint8_t)(CFG_IO_BIT(EPD_IO_VOL_UP) | CFG_IO_BIT(EPD_IO_VOL_DOWN) |
                                        CFG_IO_BIT(EPD_IO_POWER));
    s_p1_out = (uint8_t)((rail | keys_idle) & (uint8_t)~CFG_PIN_PWR_PULSE);
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    pca9555_set_config(config_reg.port, 0x00, CFG_PORT_INT);

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0);
    pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1);
    ESP_LOGI(TAG, "sleep mask all-out TPS=low keys=idle-high INT_GPIO=%d P0=0x%02X P1=0x%02X",
             gpio_get_level(CFG_INTR), p0, p1);
}

bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void) {
    if (!s_ioexp_ok) {
        return false;
    }
    // P17 锁存已是高；改输入时同电平，尽量无假边沿
    if (pca9555_set_config(config_reg.port, CFG_P1_SLEEP_WAKE_KEYS, CFG_PORT_INT) != ESP_OK) {
        ESP_LOGW(TAG, "enable P17 input fail");
        return false;
    }
    return true;
}

bool epd_board_metalio_eink4_plus_ioexp_restore_keys_checked(void) {
    s_sleep_keys_restored = false;
    if (!s_ioexp_ok) {
        return false;
    }
    // Attempt the OEM restore order once, retaining any error even if a later
    // register read happens to match. Callers must not add rail loads on failure.
    bool ok = true;
    if (pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL) != ESP_OK) ok = false;
    if (pca9555_set_config(config_reg.port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL) != ESP_OK) ok = false;

    // 保留 P12/P14 锁存；清关机脉冲；按键脚改回输入（P16 仍为 RTC_INT 输入）
    s_p1_out = (uint8_t)((s_p1_out & CFG_P1_RAIL_MASK) & (uint8_t)~CFG_PIN_PWR_PULSE);
    if (pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT) != ESP_OK) ok = false;
    if (pca9555_set_config(config_reg.port, CFG_P1_INPUT_MASK, CFG_PORT_INT) != ESP_OK) ok = false;

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    if (pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0) != ESP_OK) ok = false;
    if (pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1) != ESP_OK) ok = false;
    const bool state_ok = epd_board_metalio_eink4_plus_sleep_io_checked(false);
    ESP_LOGI(TAG, "keys restore P0=0x%02X P1=0x%02X INT_GPIO=%d ok=%d state=%d", p0, p1,
             gpio_get_level(CFG_INTR), ok, state_ok);
    s_sleep_keys_restored = ok && state_ok;
    return s_sleep_keys_restored;
}

void epd_board_metalio_eink4_plus_ioexp_restore_keys(void) {
    (void)epd_board_metalio_eink4_plus_ioexp_restore_keys_checked();
}

static void p1_pwr_pulse_set(bool high) {
    if (high) {
        s_p1_out |= CFG_PIN_PWR_PULSE;
    } else {
        s_p1_out &= (uint8_t)~CFG_PIN_PWR_PULSE;
    }
    p1_write();
}

void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "pwr pulse skipped (no TCA)");
        return;
    }
    ESP_LOGI(TAG, "pwr pulse train P1%d(io=%d) x%d interval=%dms", EPD_IO_PWR_PULSE & 7,
             EPD_IO_PWR_PULSE, PWR_PULSE_COUNT, PWR_PULSE_INTERVAL_MS);
    for (int i = 0; i < PWR_PULSE_COUNT; ++i) {
        p1_pwr_pulse_set(true);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
        p1_pwr_pulse_set(false);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
    }
}

void epd_board_metalio_eink4_plus_tp_reset(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "TP RST skipped (no TCA)");
        vTaskDelay(pdMS_TO_TICKS(300));
        return;
    }
    s_p1_out &= (uint8_t)~CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(10)); // Trst ≥ 5ms
    s_p1_out |= CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(300)); // Trsi ≥ 300ms
    ESP_LOGI(TAG, "TP RST pulse on TCA P1%d (io=%d)", EPD_IO_TP_RST & 7, EPD_IO_TP_RST);
}

/* 从设备半掉电可能钳住 SDA；legacy i2c clear_bus 也解不开时需 bit-bang */
static void recover_i2c_bus_pins(void) {
    const gpio_config_t conf = {
        .pin_bit_mask = (1ULL << CFG_SDA) | (1ULL << CFG_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_set_level(CFG_SDA, 1);
    gpio_set_level(CFG_SCL, 1);
    esp_rom_delay_us(20);

    for (int i = 0; i < 9 && gpio_get_level(CFG_SDA) == 0; ++i) {
        gpio_set_level(CFG_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(CFG_SCL, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level(CFG_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(CFG_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(CFG_SDA, 1);
    esp_rom_delay_us(5);
}

static esp_err_t i2c_probe_addr(i2c_port_t port, uint8_t addr_7bit) {
    (void)port;
    return plus_i2c_probe(addr_7bit);
}

static void i2c_scan_log(i2c_port_t port) {
    // 只 probe：对每个 ACK 地址盲读 0x08/0x0F 会拖死 BQ27220 并触发 clear bus error
    ESP_LOGI(TAG, "I2C scan SDA=%d SCL=%d (probe only):", (int)CFG_SDA, (int)CFG_SCL);
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG, "  found 0x%02X", addr);
    }
}

/** @return true 找到并配置好扩展器 */
static bool probe_and_init_ioexp(i2c_port_t port, bool panel_rail) {
    static const uint8_t k_candidates[] = {0x20, 0x24, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27};
    for (size_t i = 0; i < sizeof(k_candidates); ++i) {
        uint8_t addr = k_candidates[i];
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        pca9555_set_addr(addr);
        ESP_LOGI(TAG, "TCA/PCA9555 ACK at 0x%02X", addr);

        s_cam_scr_on = panel_rail ? CFG_PIN_CAM_SCR : 0;
        s_motor_on = 0;
        pca9555_set_motor(false);
        s_pa_on = 0;
        if (pca9555_set_value(port, p0_output_value(), CFG_PORT_CTRL) != ESP_OK) {
            continue;
        }
        // Load safe output values before enabling output drivers.
        if (pca9555_set_config(port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL) != ESP_OK) {
            continue;
        }
        /* Port1：P12/P14 输出拉高、关机脉冲默认低；P16=RTC_INT 输入 */
        s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
        if (pca9555_set_value(port, s_p1_out, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        if (pca9555_set_config(port, CFG_P1_INPUT_MASK, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG,
                 "TPS↔TCA: VCOM=P%d PWRUP=P%d WAKE=P%d PG=P%d | P5/P12/P14 out P16=RTC_IN | P0=0x%02X",
                 EPD_TPS_IO_VCOM_CTRL, EPD_TPS_IO_PWRUP, EPD_TPS_IO_WAKEUP, EPD_TPS_IO_PWR_GOOD,
                 CFG_P0_OUTPUT_MASK);
        return true;
    }
    return false;
}

bool epd_board_metalio_eink4_plus_safe_io_init(void) {
    config_reg.port = EPDIY_I2C_PORT;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;
    // Keep the manufacturer's normal P5 logic supply while disabling all TPS
    // controls. Cutting P5 here coincides with loss of the live USB console;
    // the exact board power dependency still requires confirmation.
    s_ioexp_ok = probe_and_init_ioexp(config_reg.port, true);
    return s_ioexp_ok;
}

static void epd_board_init(uint32_t epd_row_width) {
    (void)epd_row_width;
    epd_board_metalio_eink4_plus_boot_release_holds();

    if (!plus_i2c_initialized()) recover_i2c_bus_pins();

    esp_err_t err = plus_i2c_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared Wire1 init: %s", esp_err_to_name(err));
        return;
    }

    config_reg.port = EPDIY_I2C_PORT;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;

    s_ioexp_ok = probe_and_init_ioexp(config_reg.port, true);
    // 深睡曾关 P14(VOUT)：TCA 锁存仍低，上面已拉高；等轨稳定再启 TPS
    if (s_ioexp_ok) {
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    epd_board_metalio_eink4_plus_ana_sw_set(true); // GPIO36 默认高（非虚拟 U 盘）
    i2c_scan_log(config_reg.port); // 启动即扫；此时 TPS 多半仍 SLEEP，可能只有 TCA
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "TCA9555 not found; continue (panel power unavailable)");
    }

    gpio_reset_pin(CFG_INTR);
    gpio_set_direction(CFG_INTR, GPIO_MODE_INPUT);
    gpio_pullup_en(CFG_INTR);
    gpio_pulldown_dis(CFG_INTR);
    gpio_set_intr_type(CFG_INTR, GPIO_INTR_NEGEDGE);
    if (s_ioexp_irq_sem == NULL) {
        s_ioexp_irq_sem = xSemaphoreCreateBinary();
    }
    esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_EDGE);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr));
    } else {
        err = gpio_isr_handler_add(CFG_INTR, interrupt_handler, (void*)CFG_INTR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(err));
        }
    }
    // 读口清 TCA INT，避免上电残留一直拉低
    if (s_ioexp_ok) {
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }

    epd_ctrl_gpios_out_low();
    epd_lcd_reinit();
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "init ok %dx%d bus=%d pclk=%dMHz I2C SDA=%d SCL=%d ioexp=%s addr=0x%02X",
             epd_get_display()->width, epd_get_display()->height, epd_get_display()->bus_width,
             epd_get_display()->bus_speed, CFG_SDA, CFG_SCL, s_ioexp_ok ? "ok" : "FAIL",
             pca9555_get_addr());
}

static void epd_board_deinit(void) {
    gpio_set_level(EPD_OE, 0);
    gpio_set_level(EPD_MODE, 0);
    gpio_set_direction(EPD_OE, GPIO_MODE_INPUT);
    gpio_set_direction(EPD_MODE, GPIO_MODE_INPUT);

    epd_lcd_deinit();

    if (s_ioexp_ok) {
        config_reg.vcom_ctrl = false;
        config_reg.pwrup = false;
        config_reg.wakeup = false;
        s_motor_on = 0;
        pca9555_set_motor(false);
        s_pa_on = 0;
        s_cam_scr_on = 0;
        // Keep the TPS enable pins actively low; input mode has pull-ups.
        esp_err_t safe_err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
        if (safe_err != ESP_OK) ESP_LOGE(TAG, "deinit safe output failed: %s", esp_err_to_name(safe_err));
        pca9555_set_config(config_reg.port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL);

        int tries = 0;
        while ((pca9555_read_input(config_reg.port, CFG_PORT_CTRL) & CFG_PIN_PWRGOOD) != 0) {
            if (tries >= 50) {
                ESP_LOGE(TAG, "TPS65185 shutdown timeout");
                break;
            }
            tries++;
            vTaskDelay(1);
        }

        vTaskDelay(50);
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }
    // Shared Wire1 remains owned by the system (gauge/RTC/input also use it).
    gpio_isr_handler_remove(CFG_INTR);
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    if (mask->ep_output_enable) {
        gpio_set_level(EPD_OE, state->ep_output_enable ? 1 : 0);
    }
    if (mask->ep_mode) {
        gpio_set_level(EPD_MODE, state->ep_mode ? 1 : 0);
    }

    if (!s_ioexp_ok) {
        return;
    }
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) {
        esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pca9555_set_value: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 上电：PWRUP 保持低、WAKEUP 高进 STANDBY；读 REVID → 写 UPSEQ0 → 写 UPSEQ1；
 *        无论上述是否成功都拉高 PWRUP（上升沿启动升压）。GPIO PG 后再写 ENABLE/VCOM。
 */
static void epd_board_poweron(epd_ctrl_state_t* state) {
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "poweron skipped: no IO expander");
        return;
    }
    if (!epd_board_metalio_eink4_plus_bus_restore_checked()) {
        s_tps_ok = false;
        return; // Never run a waveform against a partially rebuilt LCD path.
    }
    // Restore the schematic's original VBORDER bias. GPIO37 HIGH in 2.0.67
    // did not resolve the reported flashing of addressable edge pixels.
    gpio_set_level(EPD_BORDER,0);
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };

    state->ep_output_enable = false;
    state->ep_stv = true;
    state->ep_mode = false;

    // 1) PWRUP 低 + WAKEUP 高 → STANDBY，I2C 可用
    config_reg.wakeup = true;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    epd_board_set_ctrl(state, &mask);
    // 深睡后 TPS 从 SLEEP 醒，I2C 应答前探测会失败：逐毫秒探测到应答为止（上限约 60 ms），
    // 应答后再留 2 ms 余量，不再每次翻页都固定等满 50 ms。
    // / Waking from SLEEP: probe every millisecond until the TPS answers (about 60 ms at
    // most), then allow 2 ms of margin, instead of a fixed 50 ms on every page turn.
    const int64_t power_started_us = esp_timer_get_time();
    esp_err_t probe = ESP_FAIL;
    for (int i = 0; i < 60 && probe != ESP_OK; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
        probe = tps_probe(config_reg.port);
    }
    if (probe == ESP_OK) vTaskDelay(pdMS_TO_TICKS(2));

    // 写上电时序 UPSEQ0、UPSEQ1（失败也继续）。原先每次上电的寄存器回读和日志只用于排查，
    // 已删去：PWR_GOOD 等待和失败分支仍记录完整寄存器。
    esp_err_t up0 = tps_write_register(config_reg.port, TPS_REG_UPSEQ0, 0xE4);
    esp_err_t up1 = tps_write_register(config_reg.port, TPS_REG_UPSEQ1, 0x55);
    if (up0 != ESP_OK || up1 != ESP_OK) {
        ESP_LOGW(TAG, "UPSEQ write %s/%s probe=%s", esp_err_to_name(up0), esp_err_to_name(up1),
                 esp_err_to_name(probe));
    }
    uint8_t revid = 0;  // read only on failure, below

    // 拉高 PWRUP（上升沿）；电源就绪由下面逐毫秒查 PWR_GOOD 决定，不再先固定等 50 ms。
    config_reg.pwrup = true;
    config_reg.vcom_ctrl = true;
    epd_board_set_ctrl(state, &mask);

    bool success = false;
    for (int retry = 0; retry < 3 && !success; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "TPS startup retry %d/3", retry + 1);
            config_reg.pwrup = false;
            epd_board_set_ctrl(state, &mask);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        config_reg.pwrup = true;
        config_reg.vcom_ctrl = true;
        epd_board_set_ctrl(state, &mask);

        // demo：只等扩展器 GPIO PWR_GOOD，成功前不写 ENABLE
        for (int wait_ms = 0; wait_ms < 200; ++wait_ms) {
            uint8_t p0 = pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
            if (p0 & CFG_PIN_PWRGOOD) {
                ESP_LOGD(TAG, "PWR_GOOD ok p0=0x%02X @%dms", p0, wait_ms);
                success = true;
                break;
            }
            if (wait_ms == 0 || (wait_ms % 50) == 49) {
                uint8_t en = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_ENABLE) : 0;
                uint8_t pg = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_PG) : 0;
                uint8_t int1 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT1) : 0;
                uint8_t int2 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT2) : 0;
                ESP_LOGI(TAG, "wait GPIO PG... p0=0x%02X ENABLE=0x%02X PG=0x%02X INT1=0x%02X INT2=0x%02X",
                         p0, en, pg, int1, int2);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (!success) {
        if (probe == ESP_OK) tps_read_register_ex(config_reg.port, TPS_REG_REVID, &revid);
        uint8_t p0 = pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        uint8_t en = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_ENABLE) : 0;
        uint8_t pg = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_PG) : 0;
        uint8_t int1 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT1) : 0;
        uint8_t int2 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT2) : 0;
        ESP_LOGE(TAG,
                 "TPS PowerOn failed; p0=0x%02X REVID=0x%02X ENABLE=0x%02X INT1=0x%02X INT2=0x%02X "
                 "PG=0x%02X (check VDD4.2V / L6 L7)",
                 p0, revid, en, int1, int2, pg);
        s_tps_ok = false;
        epd_board_poweroff(state);
        ESP_LOGE(TAG, "TPS initialization failed; shutdown requested");
        return;
    }

    // demo：GPIO PG 成功后再写寄存器
    if (probe == ESP_OK) {
        esp_err_t err = tps_write_register(config_reg.port, TPS_REG_ENABLE, 0x3F);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tps ENABLE write: %s", esp_err_to_name(err));
            s_tps_ok = false;
            epd_board_poweroff(state);
            return;
        }
        tps_set_vcom(config_reg.port, vcom);
    }

    state->ep_sth = true;
    epd_ctrl_state_t sth_mask = {.ep_sth = true};
    epd_board_set_ctrl(state, &sth_mask);

    int tries = 0;
    while (probe == ESP_OK && !((tps_read_register(config_reg.port, TPS_REG_PG) & 0xFA) == 0xFA)) {
        if (tries >= 200) {
            ESP_LOGE(TAG, "TPS PG register timeout");
            s_tps_ok = false;
            epd_board_poweroff(state);
            return;
        }
        tries++;
        vTaskDelay(1);
    }

    state->ep_output_enable = true;
    epd_ctrl_state_t out_mask = {.ep_output_enable = true};
    epd_board_set_ctrl(state, &out_mask);
    s_tps_ok = true;
    ESP_LOGI(TAG, "TPS PowerOn ok vcom=%d mV in %lld ms", vcom, (esp_timer_get_time() - power_started_us) / 1000);
}

static void epd_board_measure_vcom(epd_ctrl_state_t* state) {
    (void)state;
    ESP_LOGW(TAG, "measure_vcom not implemented");
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    epd_ctrl_state_t mask = {
        .ep_stv = true,
        .ep_output_enable = true,
        .ep_mode = true,
    };
    state->ep_stv = false;
    state->ep_output_enable = false;
    state->ep_mode = false;
    epd_board_set_ctrl(state, &mask);
    // E0470A14-AF 6.1: XOE LOW must precede VPOS/VNEG OFF by >=12us.
    esp_rom_delay_us(12);
    config_reg.vcom_ctrl = false;
    config_reg.pwrup = false;
    epd_board_set_ctrl(state, &mask);
    vTaskDelay(1);
    config_reg.wakeup = false;
    epd_board_set_ctrl(state, &mask);
}

static float epd_board_ambient_temperature(void) {
    return 25.0f;
}

static void set_vcom(int value) {
    vcom = value;
}

const EpdBoardDefinition epd_board_metalio_eink4_plus = {
    .init = epd_board_init,
    .deinit = epd_board_deinit,
    .set_ctrl = epd_board_set_ctrl,
    .poweron = epd_board_poweron,
    .poweroff = epd_board_poweroff,
    .measure_vcom = epd_board_measure_vcom,
    .get_temperature = epd_board_ambient_temperature,
    .set_vcom = set_vcom,
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};
