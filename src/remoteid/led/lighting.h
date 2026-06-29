#pragma once

#include "esp_err.h"

esp_err_t lighting_init(void);
void lighting_mark_transports_started(void);

// Latch a fault state: all configured external lighting outputs switch to the
// error blink pattern, overriding their normal pattern and the readiness gate.
// Intended for future compliance faults (serial validation failure, self-test
// failure, ...). The onboard RGB indicator has a matching red error via
// indicator_set_error(); a fault handler should call both.
void lighting_set_error(void);
