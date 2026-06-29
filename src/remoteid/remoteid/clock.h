#pragma once

#include <stdbool.h>
#include <stdint.h>

// Unix time of 2019-01-01T00:00:00Z: the OpenDroneID System / SystemUpdate epoch.
// It lands exactly on an hour boundary, which the seconds-after-hour helper relies on.
#define REMOTEID_ODID_EPOCH_2019 1546300800ULL

// Shared wall clock for all OpenDroneID timestamp fields. Seeded from an absolute
// UTC source (MAVLink SYSTEM_TIME today; GNSS later); until then it reports
// monotonic uptime-based fallbacks that callers should treat as low-confidence
// (e.g. encode timestamp accuracy as UNKNOWN).

// Seed from an absolute UTC time in Unix epoch seconds. Implausibly small values
// (no real time yet) are ignored. Safe to call from the MAVLink RX task.
void remoteid_clock_set_utc(uint64_t unix_seconds);

// True once a plausible UTC time has been seeded.
bool remoteid_clock_is_synced(void);

// Current UTC as Unix epoch seconds, or 0 when never synced.
uint64_t remoteid_clock_unix(void);

// Seconds since the ODID 2019 epoch (OPEN_DRONE_ID_SYSTEM / SYSTEM_UPDATE).
// Monotonic uptime fallback when unsynced.
uint32_t remoteid_clock_odid_2019(void);

// Seconds after the full UTC hour, 0..3600 (OPEN_DRONE_ID_LOCATION timestamp).
// Monotonic (not hour-aligned) fallback when unsynced.
float remoteid_clock_seconds_after_hour(void);
