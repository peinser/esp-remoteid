// Host regression test for the transmit liveness check: a declared transport
// must send within 5 s of the identity becoming usable (also before its first
// transmission, e.g. a BLE host that never syncs) and within 5 s of its previous
// transmission, including after a pause; good to arm only after every one sent.
#include "../../src/remoteid/remoteid/selftest.c"

#include <stdlib.h>

int64_t g_now_us = 1000000;
EventBits_t g_event_bits;
static bool g_ready;
bool remoteid_store_is_ready(void) { return g_ready; }
esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout)
{ (void)update; (void)timeout; return ESP_OK; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// One second: the given transports transmit, then the self-test checks.
static void second(bool ble4, bool beacon)
{
    g_now_us += 1000000;
    if (ble4) {
        remoteid_selftest_transmitted(REMOTEID_TRANSPORT_BLE4);
    }
    if (beacon) {
        remoteid_selftest_transmitted(REMOTEID_TRANSPORT_WIFI_BEACON);
    }
    check_transport_liveness();
}

static bool failed(void) { return atomic_load(&s_failed); }

int main(int argc, char **argv)
{
    // The deadline state is static, so each scenario runs in its own process.
    const char *scenario = argc > 1 ? argv[1] : "";
    remoteid_selftest_expect(REMOTEID_TRANSPORT_BLE4);
    remoteid_selftest_expect(REMOTEID_TRANSPORT_WIFI_BEACON);
    atomic_store(&s_started, true);

    for (int i = 0; i < 20; i++) {
        second(false, false);                 // no identity: transports paused, no deadline
    }
    CHECK(!failed() && !remoteid_selftest_ok());
    g_ready = true;

    if (strcmp(scenario, "first-send") == 0) {
        // BLE4 never transmits, e.g. the NimBLE host never syncs.
        for (int i = 0; i < 5; i++) {
            second(false, true);
        }
        CHECK(!failed() && !remoteid_selftest_ok());   // within its deadline, but not good to arm
        for (int i = 0; i < 2; i++) {
            second(false, true);
        }
        CHECK(failed());
    } else if (strcmp(scenario, "resume") == 0) {
        for (int i = 0; i < 10; i++) {
            second(true, true);
        }
        CHECK(!failed() && remoteid_selftest_ok());
        g_ready = false;                               // identity lost: paused
        for (int i = 0; i < 30; i++) {
            second(false, false);
        }
        CHECK(!failed());
        g_ready = true;                                // resumed, Wi-Fi hangs
        for (int i = 0; i < 5; i++) {
            second(true, false);
        }
        CHECK(!failed());
        for (int i = 0; i < 2; i++) {
            second(true, false);
        }
        CHECK(failed());
    } else {
        printf("usage: %s first-send|resume\n", argv[0]);
        return 2;
    }
    printf("selftest_test (%s): %s\n", scenario, failures ? "FAILED" : "passed");
    return failures != 0;
}
