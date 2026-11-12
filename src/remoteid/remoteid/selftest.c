#include "selftest.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "indicator.h"
#include "lighting.h"
#include "store.h"

#define SELFTEST_TASK_STACK    3072
#define SELFTEST_TASK_PRIORITY 4
#define SELFTEST_PERIOD_MS     2000

static const char *TAG = "remoteid_selftest";

static _Atomic bool s_started;
static _Atomic bool s_failed;

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
