#include "flash_enc_guard.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota/ota.h"
#include "remoteid/auth.h"
#include "remoteid/ble.h"
#include "remoteid/dronecan.h"
#include "led/lighting.h"
#include "remoteid/mavlink.h"
#include "remoteid/model.h"
#include "remoteid/selftest.h"
#include "remoteid/store.h"
#include "remoteid/wifi.h"
#include "led/indicator.h"
#include "sdkconfig.h"

static const char *TAG = "remoteid_main";

// Hidden in strict compliance mode: a module that reboots in flight must resume
// broadcasting at once.
#ifndef CONFIG_REMOTEID_STARTUP_DELAY_MS
#define CONFIG_REMOTEID_STARTUP_DELAY_MS 0
#endif

void app_main(void)
{
    static remoteid_state_t state;

    // The status LED and lights are not part of Remote ID: a failure is logged
    // instead of stopping the broadcast in a boot loop.
    if (indicator_init() != ESP_OK) {
        ESP_LOGE(TAG, "status indicator failed to start");
    }
    // NVS is not erased on failure: it holds the provisioned signing key. The
    // fault is reported and the device keeps broadcasting.
    remoteid_selftest_report("NVS", nvs_flash_init() == ESP_OK);
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(remoteid_ota_check_and_run());
    ESP_ERROR_CHECK(remoteid_model_init(&state));
    ESP_ERROR_CHECK(remoteid_store_start(&state));
    if (lighting_init() != ESP_OK) {
        ESP_LOGE(TAG, "lighting outputs failed to start");
    }

#if CONFIG_REMOTEID_COMPLIANCE_STRICT && !(CONFIG_SECURE_BOOT && CONFIG_SECURE_FLASH_ENC_ENABLED)
    // EU 2019/945 asks to reduce the ability to tamper; without both, the serial
    // number and position handling can be changed by reflashing the module.
    ESP_LOGW(TAG, "strict compliance build without Secure Boot and flash encryption: "
                  "not tamper-resistant, do not use for operation");
#endif

    // From here on, failures are reported to the self-test (error indicator,
    // system-failure status, arming blocked) instead of aborting into a boot
    // loop, so the device still broadcasts on every healthy path.
#if CONFIG_REMOTEID_AUTH_ED25519
    remoteid_selftest_report("Ed25519 signing", remoteid_auth_init() == ESP_OK);
#endif

#if CONFIG_REMOTEID_MAVLINK_INPUT
    remoteid_selftest_report("MAVLink", remoteid_mavlink_start() == ESP_OK);
#endif

#if CONFIG_REMOTEID_DRONECAN_INPUT
    remoteid_selftest_report("DroneCAN", remoteid_dronecan_start() == ESP_OK);
#endif

    if (CONFIG_REMOTEID_STARTUP_DELAY_MS > 0) {
        ESP_LOGI(TAG, "delaying Remote ID transport startup for %d ms", CONFIG_REMOTEID_STARTUP_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_STARTUP_DELAY_MS));
    }

#if CONFIG_REMOTEID_TRANSPORT_BLE || CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE
    remoteid_selftest_report("BLE", remoteid_ble_start() == ESP_OK);
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON || CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
    remoteid_selftest_report("Wi-Fi", remoteid_wifi_start() == ESP_OK);
#endif

    lighting_mark_transports_started();
    remoteid_selftest_start();
}
