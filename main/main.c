// ESP32-S3 prototype firmware for the WLtoys 6401 1:64 FPV car.
//
// Drives the car's internal yellow signal wire (formerly driven by the
// WiFi/camera module) directly from four push buttons. Protocol details are
// fully derived in PROTOCOL_ANALYSIS.md; the design rationale for every
// decision below is in IMPLEMENTATION_PLAN.md.
//
//   UART1 TX -> GPIO4 : 4800 8N1, non-inverted, open-drain, idle
//                       released (Hi-Z), no RX.
//   GPIO5..8          : Forward, Reverse, Left, Right buttons (active-low).
//
// Open-drain rationale: with the original module disconnected, the car's
// own board was measured pulling the yellow wire to ~2.81 V -- the car
// provides the pull-up. The ESP32 must never actively drive the line HIGH;
// it only pulls LOW (0 bits) and otherwise releases (1 bits / idle) so the
// car's pull-up sets the HIGH level. See PROTOCOL_ANALYSIS.md §11.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/uart_periph.h"

#include "protocol.h"
#include "buttons.h"

static const char *TAG = "wl6401";

#define SIGNAL_UART UART_NUM_1
#define SIGNAL_TXD_PIN GPIO_NUM_4

// Original WiFi/camera module holds the yellow wire HIGH and silent before
// its control link goes active, then transitions straight into periodic
// neutral UART traffic (PROTOCOL_ANALYSIS.md §10). Reproduced here in case
// the car's control-link startup logic depends on that silence-to-traffic
// transition.
#define SIGNAL_HOLD_HIGH_MS 5000

#define CONTROL_PERIOD_MS 50
#define STARTUP_NEUTRAL_WINDOW_MS 1000

// Verbose per-frame logging is off by default to keep the 50 ms loop light;
// flip to 1 for bring-up debugging (IMPLEMENTATION_PLAN.md §10).
#define VERBOSE_FRAME_LOG 0

// Asserts the packet builder reproduces every reference packet captured in
// PROTOCOL_ANALYSIS.md §6-7 from field values alone. Returns true if all
// five match byte-for-byte.
static bool protocol_self_test(void) {
    static const struct {
        const char *name;
        uint8_t steer;
        uint8_t thr;
        uint8_t expected[PROTOCOL_PACKET_LEN];
    } cases[] = {
        { "idle",     STEER_CENTER, THR_NEUTRAL, { 0x66, 0x80, 0x80, 0x80, 0x00, 0x00, 0x80, 0x99 } },
        { "forward",  STEER_CENTER, THR_FORWARD, { 0x66, 0x80, 0xFF, 0x80, 0x00, 0x00, 0xFF, 0x99 } },
        { "backward", STEER_CENTER, THR_REVERSE, { 0x66, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x99 } },
        { "left",     STEER_LEFT,   THR_NEUTRAL, { 0x66, 0x59, 0x80, 0x80, 0x00, 0x00, 0x59, 0x99 } },
        { "right",    STEER_RIGHT,  THR_NEUTRAL, { 0x66, 0xA6, 0x80, 0x80, 0x00, 0x00, 0xA6, 0x99 } },
    };

    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t out[PROTOCOL_PACKET_LEN];
        protocol_build(cases[i].steer, cases[i].thr, out);
        if (memcmp(out, cases[i].expected, PROTOCOL_PACKET_LEN) != 0) {
            ESP_LOGE(TAG, "self-test FAILED for '%s' case", cases[i].name);
            ok = false;
        }
    }
    return ok;
}

// Releases GPIO4 (open-drain, output level 1 = Hi-Z) with no UART peripheral
// attached to the pin yet -- so nothing is transmitted while this holds and
// the car's own pull-up is free to hold the line at its native ~2.81 V.
// Called first so no spurious LOW/start-bit glitch is emitted before or
// during UART init (IMPLEMENTATION_PLAN.md §3, §7).
//
// GPIO_MODE_OUTPUT_OD sets the pin's pad_driver bit (GPIO_PINn_REG), which
// selects open-drain vs. push-pull for that pad's output stage. That bit is
// independent of which internal signal drives the pad -- plain GPIO here,
// or the UART1 TX matrix signal after signal_uart_start() below -- so it
// stays in effect across the switch and never needs to be set twice.
static void signal_line_hold_high(void) {
    gpio_config_t idle_cfg = {
        .pin_bit_mask = 1ULL << SIGNAL_TXD_PIN,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,  // car provides the pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&idle_cfg);
    gpio_set_level(SIGNAL_TXD_PIN, 1);  // open-drain 1 = release (Hi-Z)
}

// Attaches the UART1 peripheral to the signal pin. Call only after the
// silent HIGH hold above (signal_line_hold_high()) so the pin is already
// resting released at the correct idle/mark level before the peripheral
// takes it over.
static void signal_uart_start(void) {
    const uart_config_t uart_cfg = {
        .baud_rate = 4800,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SIGNAL_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SIGNAL_UART, &uart_cfg));
    // Non-inverted: leave UART_SIGNAL_* inversion defaults untouched.
    // RX not assigned -- the captures show no reply traffic and the
    // reference repo never reads it.
    ESP_ERROR_CHECK(uart_set_pin(SIGNAL_UART, SIGNAL_TXD_PIN, UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // uart_set_pin() leaves pad_driver alone, so the open-drain mode set by
    // signal_line_hold_high()'s gpio_config() already persists here -- this
    // gpio_set_direction() call is a redundant but harmless re-assert of
    // that same bit. Its side effect is the problem: it also re-routes the
    // pad to the plain-GPIO matrix signal (SIG_GPIO_OUT_IDX), silently
    // disconnecting UART1 TX. Reconnect UART1 TX explicitly right after so
    // the final pad state is deterministic regardless of call order.
    ESP_ERROR_CHECK(gpio_set_direction(SIGNAL_TXD_PIN, GPIO_MODE_OUTPUT_OD));
    esp_rom_gpio_connect_out_signal(SIGNAL_TXD_PIN,
                                     UART_PERIPH_SIGNAL(SIGNAL_UART, SOC_UART_TX_PIN_IDX),
                                     false, false);
}

static void buttons_poll_task(void *arg) {
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(BUTTONS_POLL_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        buttons_poll();
        vTaskDelayUntil(&last_wake, period);
    }
}

// Applies the conflict-resolution rules from IMPLEMENTATION_PLAN.md §5:
// opposite buttons held together, or neither held, both resolve to neutral.
static void resolve_fields(const button_state_t *btn, uint8_t *steer, uint8_t *thr) {
    *thr = THR_NEUTRAL;
    if (btn->forward && !btn->reverse) {
        *thr = THR_FORWARD;
    } else if (btn->reverse && !btn->forward) {
        *thr = THR_REVERSE;
    }

    *steer = STEER_CENTER;
    if (btn->left && !btn->right) {
        *steer = STEER_LEFT;
    } else if (btn->right && !btn->left) {
        *steer = STEER_RIGHT;
    }
}

static void control_task(void *arg) {
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);
    const TickType_t start_tick = xTaskGetTickCount();
    TickType_t last_wake = start_tick;

    uint8_t last_packet[PROTOCOL_PACKET_LEN];
    memset(last_packet, 0, sizeof(last_packet));
    bool have_last_packet = false;

    for (;;) {
        uint32_t elapsed_ms = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;

        uint8_t steer, thr;
        if (elapsed_ms < STARTUP_NEUTRAL_WINDOW_MS) {
            // Startup neutral window: ignore button input entirely while the
            // debouncer fills its history and the line settles.
            steer = STEER_CENTER;
            thr = THR_NEUTRAL;
        } else {
            button_state_t btn = buttons_get_state();
            resolve_fields(&btn, &steer, &thr);
        }

        uint8_t packet[PROTOCOL_PACKET_LEN];
        protocol_build(steer, thr, packet);
        uart_write_bytes(SIGNAL_UART, (const char *)packet, PROTOCOL_PACKET_LEN);

        if (VERBOSE_FRAME_LOG || !have_last_packet || memcmp(packet, last_packet, PROTOCOL_PACKET_LEN) != 0) {
            ESP_LOGI(TAG, "steer=0x%02X thr=0x%02X pkt=%02X %02X %02X %02X %02X %02X %02X %02X",
                     steer, thr,
                     packet[0], packet[1], packet[2], packet[3],
                     packet[4], packet[5], packet[6], packet[7]);
            memcpy(last_packet, packet, PROTOCOL_PACKET_LEN);
            have_last_packet = true;
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

void app_main(void) {
    if (!protocol_self_test()) {
        ESP_LOGE(TAG, "protocol builder self-test failed; halting before any UART output");
        // Never proceed to transmit -- just hold the line HIGH forever.
        signal_line_hold_high();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "protocol builder self-test passed");

    // Reproduce the original WiFi/camera module's control-link startup: hold
    // the signal line HIGH and silent (no UART, no frames) for a fixed window
    // before any traffic begins (PROTOCOL_ANALYSIS.md §10).
    signal_line_hold_high();
    ESP_LOGI(TAG, "signal line HIGH, silent for %d ms (control-link startup hold)", SIGNAL_HOLD_HIGH_MS);
    vTaskDelay(pdMS_TO_TICKS(SIGNAL_HOLD_HIGH_MS));

    signal_uart_start();
    buttons_init();

    xTaskCreate(buttons_poll_task, "buttons_poll", 2048, NULL, 6, NULL);
    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "running: %d ms post-UART-start neutral window, then %d ms control cadence",
             STARTUP_NEUTRAL_WINDOW_MS, CONTROL_PERIOD_MS);
}
