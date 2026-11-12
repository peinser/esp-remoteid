#include "selftest.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "indicator.h"
#include "lighting.h"
#include "store.h"

#define SELFTEST_TASK_STACK    3072
#define SELFTEST_TASK_PRIORITY 4
#define SELFTEST_PERIOD_MS     2000
// Longest a running transport may go without a successful transmission. Every
// message is due at least every 3 s; the margin avoids latching on a short stall.
#define SELFTEST_TX_TIMEOUT_MS 5000

static const char *TAG = "remoteid_selftest";

static _Atomic bool s_started;
static _Atomic bool s_failed;
// esp_timer milliseconds of each transport's last successful transmission;
// 0 while it has not started or is paused.
static _Atomic uint32_t s_last_tx_ms[REMOTEID_TRANSPORT_COUNT];
static const char *const TRANSPORT_NAMES[REMOTEID_TRANSPORT_COUNT] = {
    "BLE4", "BLE5", "Wi-Fi Beacon", "Wi-Fi NAN",
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void remoteid_selftest_transmitted(remoteid_transport_t transport)
{
    // Never store 0: it means paused.
    uint32_t now = now_ms();
    atomic_store_explicit(&s_last_tx_ms[transport], now ? now : 1, memory_order_relaxed);
}

void remoteid_selftest_paused(remoteid_transport_t transport)
{
    atomic_store_explicit(&s_last_tx_ms[transport], 0, memory_order_relaxed);
}

static void check_transport_liveness(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < REMOTEID_TRANSPORT_COUNT; i++) {
        uint32_t last = atomic_load_explicit(&s_last_tx_ms[i], memory_order_relaxed);
        if (last != 0 && now - last > SELFTEST_TX_TIMEOUT_MS) {
            ESP_LOGE(TAG, "%s has not transmitted for %u ms", TRANSPORT_NAMES[i], (unsigned)(now - last));
            remoteid_selftest_report(TRANSPORT_NAMES[i], false);
            atomic_store_explicit(&s_last_tx_ms[i], 0, memory_order_relaxed); // report once
        }
    }
}

void remoteid_selftest_report(const char *component, bool ok)
{
    if (ok) {
        ESP_LOGI(TAG, "'%s' healthy", component);
        return;
    }
    ESP_LOGE(TAG, "'%s' failed", component);
    atomic_store_explicit(&s_failed, true, memory_order_release);
}

bool remoteid_selftest_ok(void)
{
    return atomic_load_explicit(&s_started, memory_order_acquire) &&
           !atomic_load_explicit(&s_failed, memory_order_acquire);
}

static bool raise_fault(void)
{
    indicator_set_error();
    lighting_set_error();
    // Force the broadcast status to system-failure (the store keeps it sticky
    // against later LOCATION updates); ARM_STATUS follows from the store health.
    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_SELFTEST_FAILED };
    return remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK;
}

static void selftest_task(void *arg)
{
    (void)arg;
    bool fault_raised = false;

    while (true) {
        check_transport_liveness();
        // Retried every period until the store has accepted the fault.
        if (atomic_load_explicit(&s_failed, memory_order_acquire) && !fault_raised) {
            ESP_LOGE(TAG, "self-test FAILED: raising error indicator and system-failure status");
            fault_raised = raise_fault();
        }
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_PERIOD_MS));
    }
}

void remoteid_selftest_start(void)
{
    atomic_store_explicit(&s_started, true, memory_order_release);
    if (xTaskCreate(selftest_task, "remoteid_selftest", SELFTEST_TASK_STACK,
                    NULL, SELFTEST_TASK_PRIORITY, NULL) != pdPASS) {
        // Without the monitor a fault could never be latched: report not-ok.
        ESP_LOGE(TAG, "failed to create self-test task");
        atomic_store_explicit(&s_failed, true, memory_order_release);
    }
}
