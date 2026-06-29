#pragma once

#include <stdbool.h>

// Record a transport's initialisation result. Call once per transport right after
// remoteid_ble_start() / remoteid_wifi_start().
void remoteid_selftest_report_transport(const char *name, bool ok);

// True while the Remote ID system is healthy (all reported transports initialised
// and no runtime fault detected). Consumed by the ARM_STATUS / arm-readiness path.
bool remoteid_selftest_ok(void);

// Start the self-test monitor. On failure it raises the error indicator and pushes
// a Remote ID system-failure into the store (broadcast status -> system failure,
// arming blocked). Call after the transports have been started.
void remoteid_selftest_start(void);
