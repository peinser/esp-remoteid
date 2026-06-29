#include "clock.h"

#include "esp_timer.h"

// The ODID-2019 second that corresponded to esp_timer t=0. A single 32-bit word
// so reads/writes stay atomic across the RX task (writer) and the broadcast /
// operator-location tasks (readers); a torn read is impossible on a 32-bit load.
static volatile uint32_t s_epoch2019_at_boot;
static volatile bool     s_synced;

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

void remoteid_clock_set_utc(uint64_t unix_seconds)
{
    if (unix_seconds <= REMOTEID_ODID_EPOCH_2019) {
        return; // no real wall clock yet
    }
    s_epoch2019_at_boot = (uint32_t)(unix_seconds - REMOTEID_ODID_EPOCH_2019) - uptime_seconds();
    s_synced = true;
}

bool remoteid_clock_is_synced(void)
{
    return s_synced;
}

uint32_t remoteid_clock_odid_2019(void)
{
    if (!s_synced) {
        return uptime_seconds(); // monotonic fallback
    }
    return s_epoch2019_at_boot + uptime_seconds();
}

uint64_t remoteid_clock_unix(void)
{
    if (!s_synced) {
        return 0;
    }
    return REMOTEID_ODID_EPOCH_2019 + (uint64_t)remoteid_clock_odid_2019();
}

float remoteid_clock_seconds_after_hour(void)
{
    // The 2019 epoch is hour-aligned, so (unix % 3600) == (odid_2019 % 3600).
    return (float)(remoteid_clock_odid_2019() % 3600);
}
