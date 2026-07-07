#include "command.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "protocol.h"
#include "config.h"

// Guards the desired-command fields, written by the WebSocket handler task and
// read by control_task -- mirrors the portMUX approach the old buttons module
// used for its cross-task state.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_steer = STEER_CENTER;
static uint8_t s_thr = THR_NEUTRAL;
// Tick of the last command_set(). 0 means "never set"; treated as stale so the
// failsafe holds neutral until the first real command arrives.
static TickType_t s_last_tick = 0;

void command_init(void) {
    portENTER_CRITICAL(&s_lock);
    s_steer = STEER_CENTER;
    s_thr = THR_NEUTRAL;
    s_last_tick = 0;
    portEXIT_CRITICAL(&s_lock);
}

void command_set(uint8_t steer, uint8_t thr) {
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_lock);
    s_steer = steer;
    s_thr = thr;
    s_last_tick = now;
    portEXIT_CRITICAL(&s_lock);
}

void command_get(uint8_t *steer, uint8_t *thr) {
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_lock);
    uint8_t s = s_steer;
    uint8_t t = s_thr;
    TickType_t last = s_last_tick;
    portEXIT_CRITICAL(&s_lock);

    uint32_t age_ms = (last == 0) ? UINT32_MAX
                                  : (uint32_t)(now - last) * portTICK_PERIOD_MS;
    if (age_ms >= CONTROL_FAILSAFE_MS) {
        s = STEER_CENTER;
        t = THR_NEUTRAL;
    }
    *steer = s;
    *thr = t;
}

uint32_t command_age_ms(void) {
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_lock);
    TickType_t last = s_last_tick;
    portEXIT_CRITICAL(&s_lock);
    if (last == 0) {
        return UINT32_MAX;
    }
    return (uint32_t)(now - last) * portTICK_PERIOD_MS;
}
