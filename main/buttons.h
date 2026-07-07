#pragma once

#include <stdbool.h>

// Debounced active-low push-button reading for the four direction buttons.
//
// Pins, per IMPLEMENTATION_PLAN.md §6:
//   GPIO5 = Forward, GPIO6 = Reverse, GPIO7 = Left, GPIO8 = Right.
// Each pin uses the internal pull-up; wiring is GPIO -> button -> shared GND,
// so "not pressed" reads HIGH and "pressed" reads LOW.

typedef struct {
    bool forward;
    bool reverse;
    bool left;
    bool right;
} button_state_t;

// Configures GPIO5-8 as inputs with internal pull-ups. Call once at boot.
void buttons_init(void);

// Reads the raw GPIO levels and advances the per-pin debounce integrators.
// Call periodically (every BUTTONS_POLL_INTERVAL_MS) from a dedicated task;
// non-blocking.
void buttons_poll(void);

// Returns the latest debounced state. Safe to call from a different task
// than the one calling buttons_poll().
button_state_t buttons_get_state(void);

// Poll interval this module is designed around (see buttons.c for the
// debounce integrator sizing).
#define BUTTONS_POLL_INTERVAL_MS 5
