#include "clock.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// Unix microseconds that corresponded to esp_timer t=0. Written by the MAVLink
// RX task and read by the broadcast tasks, so the 64-bit value is guarded.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_unix_us_at_boot;
static bool s_synced;

void remoteid_clock_set_utc_us(uint64_t unix_us)
{
    if (unix_us <= REMOTEID_ODID_EPOCH_2019 * 1000000ULL) {
        return; // no real wall clock yet
    }
    int64_t offset = (int64_t)unix_us - esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_unix_us_at_boot = offset;
    s_synced = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool unix_us_now(int64_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool synced = s_synced;
    int64_t offset = s_unix_us_at_boot;
    portEXIT_CRITICAL(&s_lock);

    *out = offset + esp_timer_get_time();
    return synced;
}

bool remoteid_clock_is_synced(void)
{
    int64_t unused;
    return unix_us_now(&unused);
}

uint32_t remoteid_clock_odid_2019(void)
{
    int64_t unix_us;
    if (!unix_us_now(&unix_us)) {
        return 0;
    }
    return (uint32_t)(unix_us / 1000000 - (int64_t)REMOTEID_ODID_EPOCH_2019);
}
