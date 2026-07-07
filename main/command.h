#pragma once

#include <stdbool.h>
#include <stdint.h>

// Shared desired-command state, replacing the old buttons module as the car's
// control source. The WebSocket handler writes it; the 50 ms control_task
// reads it. Values are the raw protocol bytes from protocol.h (STEER_*/THR_*),
// so no translation happens in the control loop.

// Initializes the command state to neutral. Call once at boot before any
// command_set()/command_get().
void command_init(void);

// Stores a fresh desired command and stamps it with the current tick. Called
// by the WebSocket handler on each valid control frame. Thread-safe.
void command_set(uint8_t steer, uint8_t thr);

// Returns the current desired command via *steer/*thr, or neutral if the last
// command_set() is older than CONTROL_FAILSAFE_MS (failsafe). Thread-safe.
void command_get(uint8_t *steer, uint8_t *thr);

// Milliseconds since the last command_set(), for /status reporting. Returns a
// large value if no command has ever been received.
uint32_t command_age_ms(void);
