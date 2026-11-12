#pragma once

#include <stdbool.h>
#include <stdint.h>

// Unix time of 2019-01-01T00:00:00Z: the OpenDroneID System / SystemUpdate epoch.
#define REMOTEID_ODID_EPOCH_2019 1546300800ULL

// Shared UTC clock for every OpenDroneID timestamp, both broadcast over RF and
// sent back over MAVLink. It is seeded from the bound autopilot's SYSTEM_TIME.
// Until then there is no UTC: callers get "unknown" values, never uptime.

// Seed from an absolute UTC time in Unix epoch microseconds. Values before 2019
// (no real time yet) are ignored.
void remoteid_clock_set_utc_us(uint64_t unix_us);

// True once a plausible UTC time has been seeded.
bool remoteid_clock_is_synced(void);

// Seconds since the ODID 2019 epoch (System, SystemUpdate, Authentication), or 0
// when unsynced.
uint32_t remoteid_clock_odid_2019(void);

