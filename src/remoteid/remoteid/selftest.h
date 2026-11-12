#pragma once

#include <stdbool.h>

// Record a component's health: transport initialisation, key loading, or a
// persistent runtime transmit/encode failure. A single failure latches the
// Remote ID system-failure state.
void remoteid_selftest_report(const char *component, bool ok);

// Broadcast transports, for the transmit liveness check.
typedef enum remoteid_transport {
    REMOTEID_TRANSPORT_BLE4,
    REMOTEID_TRANSPORT_BLE5,
    REMOTEID_TRANSPORT_WIFI_BEACON,
    REMOTEID_TRANSPORT_WIFI_NAN,
    REMOTEID_TRANSPORT_COUNT,
} remoteid_transport_t;

// A transport reports each successful transmission, and when it pauses (no
// usable identity yet). The self-test latches a failure when a transport that
// is not paused has sent nothing for 5 s: a hung task or radio driver would
// otherwise go unnoticed while ARM_STATUS still reports good to arm.
void remoteid_selftest_transmitted(remoteid_transport_t transport);
void remoteid_selftest_paused(remoteid_transport_t transport);

// True once the transports have been started and no fault has been reported.
// Part of the readiness reported over ARM_STATUS and on the status indicator.
bool remoteid_selftest_ok(void);

// Start the self-test monitor. On failure it raises the error indicator and pushes
// a Remote ID system-failure into the store (broadcast status -> system failure,
// arming blocked). Call after the transports have been started.
void remoteid_selftest_start(void);
