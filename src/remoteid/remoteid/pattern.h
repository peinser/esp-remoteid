#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum remoteid_pattern {
    REMOTEID_PATTERN_SOLID,
    REMOTEID_PATTERN_BEACON_1HZ_SHORT,
    REMOTEID_PATTERN_BEACON_1HZ_50,
    REMOTEID_PATTERN_STROBE_SINGLE,
    REMOTEID_PATTERN_STROBE_DOUBLE,
    REMOTEID_PATTERN_STROBE_TRIPLE,
    REMOTEID_PATTERN_FAST_STROBE,
} remoteid_pattern_t;

typedef struct remoteid_pattern_step {
    bool on;
    uint16_t duration_ms;
} remoteid_pattern_step_t;

const remoteid_pattern_step_t *remoteid_pattern_steps(remoteid_pattern_t pattern, size_t *count);
uint32_t remoteid_pattern_period_ms(remoteid_pattern_t pattern);
bool remoteid_pattern_is_on(remoteid_pattern_t pattern, uint32_t elapsed_ms, uint32_t phase_ms);
