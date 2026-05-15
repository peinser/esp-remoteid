#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum led_pattern {
    LED_PATTERN_SOLID,
    LED_PATTERN_BEACON_1HZ_SHORT,
    LED_PATTERN_BEACON_1HZ_50,
    LED_PATTERN_STROBE_SINGLE,
    LED_PATTERN_STROBE_DOUBLE,
    LED_PATTERN_STROBE_TRIPLE,
    LED_PATTERN_FAST_STROBE,
} led_pattern_t;

typedef struct led_pattern_step {
    bool on;
    uint16_t duration_ms;
} led_pattern_step_t;

const led_pattern_step_t *led_pattern_steps(led_pattern_t pattern, size_t *count);
uint32_t led_pattern_period_ms(led_pattern_t pattern);
bool led_pattern_is_on(led_pattern_t pattern, uint32_t elapsed_ms, uint32_t phase_ms);
