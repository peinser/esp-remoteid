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

static _Atomic bool s_failed;

void remoteid_selftest_report_transport(const char *name, bool ok)
{
    if (ok) {
        ESP_LOGI(TAG, "transport '%s' initialised", name ? name : "?");
        return;
    }
    ESP_LOGE(TAG, "transport '%s' failed to initialise", name ? name : "?");
    atomic_store_explicit(&s_failed, true, memory_order_release);
}

bool remoteid_selftest_ok(void)
{
    return !atomic_load_explicit(&s_failed, memory_order_acquire);
}

static void raise_fault(void)
{
    // (a) error pattern on both LED subsystems
    indicator_set_error();
    lighting_set_error();
    // (c) force the broadcast operational status to system-failure (the store also
    //     keeps it sticky against later LOCATION updates) and surface it to the
    //     readiness/ARM_STATUS path. (b) ARM_STATUS -> PRE_ARM_FAIL follows from health.
    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_SELFTEST_FAILED };
    remoteid_store_submit(&update, pdMS_TO_TICKS(100));
}

static void selftest_task(void *arg)
{
    (void)arg;
    bool fault_raised = false;

    while (true) {
        // Periodic health check. Currently this re-evaluates the recorded transport
        // init results; advertising-liveness / queue-health hooks can be added here.
        if (!remoteid_selftest_ok() && !fault_raised) {
            ESP_LOGE(TAG, "self-test FAILED: raising error indicator and system-failure status");
            raise_fault();
            fault_raised = true;
        }
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_PERIOD_MS));
    }
}

void remoteid_selftest_start(void)
{
    if (xTaskCreate(selftest_task, "remoteid_selftest", SELFTEST_TASK_STACK,
                    NULL, SELFTEST_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create self-test task");
    }
}
