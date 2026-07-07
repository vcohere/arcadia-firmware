#include "buttons.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define PIN_FORWARD GPIO_NUM_5
#define PIN_REVERSE GPIO_NUM_6
#define PIN_LEFT    GPIO_NUM_7
#define PIN_RIGHT   GPIO_NUM_8

// Integrator debounce: a pin's raw level must hold steady for this many
// consecutive polls before the debounced state flips, i.e. ~20 ms stable
// per IMPLEMENTATION_PLAN.md §6.
#define DEBOUNCE_STABLE_MS 20
#define DEBOUNCE_STEPS (DEBOUNCE_STABLE_MS / BUTTONS_POLL_INTERVAL_MS)

typedef struct {
    gpio_num_t pin;
    int integrator; // 0..DEBOUNCE_STEPS
    bool debounced; // true = pressed
} debounced_button_t;

static debounced_button_t s_buttons[4] = {
    { .pin = PIN_FORWARD },
    { .pin = PIN_REVERSE },
    { .pin = PIN_LEFT },
    { .pin = PIN_RIGHT },
};

// Guards the .debounced fields, which buttons_poll() (one task) writes and
// buttons_get_state() (a different task) reads.
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

void buttons_init(void) {
    for (int i = 0; i < 4; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_buttons[i].pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }
}

void buttons_poll(void) {
    for (int i = 0; i < 4; i++) {
        bool raw_pressed = gpio_get_level(s_buttons[i].pin) == 0; // active-low

        int integrator = s_buttons[i].integrator;
        if (raw_pressed) {
            if (integrator < DEBOUNCE_STEPS) {
                integrator++;
            }
        } else {
            if (integrator > 0) {
                integrator--;
            }
        }
        s_buttons[i].integrator = integrator;

        // Hysteresis: only the two extremes update the debounced state: a
        // run of fully-pressed or fully-released polls. Anything in between
        // (a bounce mid-transition) leaves the previous debounced value.
        bool debounced = s_buttons[i].debounced;
        if (integrator >= DEBOUNCE_STEPS) {
            debounced = true;
        } else if (integrator <= 0) {
            debounced = false;
        }

        if (debounced != s_buttons[i].debounced) {
            portENTER_CRITICAL(&s_state_lock);
            s_buttons[i].debounced = debounced;
            portEXIT_CRITICAL(&s_state_lock);
        }
    }
}

button_state_t buttons_get_state(void) {
    button_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state.forward = s_buttons[0].debounced;
    state.reverse = s_buttons[1].debounced;
    state.left    = s_buttons[2].debounced;
    state.right   = s_buttons[3].debounced;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}
