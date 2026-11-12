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

// A transport declares itself when it starts and reports each successful
// transmission. While there is a usable identity (the transports only send
// then), every declared transport must send within 5 s of the identity
// becoming usable and then at least every 5 s; otherwise the self-test latches
// a failure. This catches a task or radio driver that hangs before its first
// transmission as well as one that stops later.
void remoteid_selftest_expect(remoteid_transport_t transport);
void remoteid_selftest_transmitted(remoteid_transport_t transport);

// True once the transports have been started, each of them has transmitted,
// and no fault has been reported.
// Part of the readiness reported over ARM_STATUS and on the status indicator.
bool remoteid_selftest_ok(void);

// Start the self-test monitor. On failure it raises the error indicator and pushes
// a Remote ID system-failure into the store (broadcast status -> system failure,
// arming blocked). Call after the transports have been started.
void remoteid_selftest_start(void);
