#pragma once
#include <stdint.h>
// Tests advance this to move time.
extern int64_t g_now_us;
static inline int64_t esp_timer_get_time(void) { return g_now_us; }
