#pragma once

#include <stdbool.h>

// Record a component's health: transport initialisation, key loading, or a
// persistent runtime transmit/encode failure. A single failure latches the
// Remote ID system-failure state.
void remoteid_selftest_report(const char *component, bool ok);

// True once the transports have been started and no fault has been reported.
// Part of the readiness reported over ARM_STATUS and on the status indicator.
bool remoteid_selftest_ok(void);

// Start the self-test monitor. On failure it raises the error indicator and pushes
// a Remote ID system-failure into the store (broadcast status -> system failure,
// arming blocked). Call after the transports have been started.
void remoteid_selftest_start(void);
