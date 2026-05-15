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
#include "led/indicator.h"
#include "led/lighting.h"
#include "remoteid/mavlink.h"
#include "remoteid/model.h"
#include "remoteid/store.h"
#include "remoteid/wifi.h"
#include "sdkconfig.h"

static const char *TAG = "remoteid_main";

#ifndef CONFIG_REMOTEID_STARTUP_DELAY_MS
#define CONFIG_REMOTEID_STARTUP_DELAY_MS 10000
#endif

void app_main(void)
{
    static remoteid_state_t state;

    ESP_ERROR_CHECK(indicator_init());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(remoteid_ota_check_and_run());
    ESP_ERROR_CHECK(remoteid_model_init(&state));
    ESP_ERROR_CHECK(remoteid_store_start(&state));
    ESP_ERROR_CHECK(lighting_init());
    ESP_ERROR_CHECK(remoteid_auth_init());

#if CONFIG_REMOTEID_MAVLINK_INPUT
    ESP_ERROR_CHECK(remoteid_mavlink_start());
#endif

    if (CONFIG_REMOTEID_STARTUP_DELAY_MS > 0) {
        ESP_LOGI(TAG, "delaying Remote ID transport startup for %d ms", CONFIG_REMOTEID_STARTUP_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_STARTUP_DELAY_MS));
    }

#if CONFIG_REMOTEID_TRANSPORT_BLE
    ESP_ERROR_CHECK(remoteid_ble_start());
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON || CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
    ESP_ERROR_CHECK(remoteid_wifi_start());
#endif

    indicator_mark_transports_started();
    lighting_mark_transports_started();
}
