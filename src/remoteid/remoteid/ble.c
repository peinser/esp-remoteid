#include "ble.h"

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "encode.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "led.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "opendroneid.h"
#include "sdkconfig.h"
#include "store.h"

#define REMOTEID_SERVICE_DATA_LEN (2 + ODID_MESSAGE_SIZE)
#define REMOTEID_LEGACY_ADV_DATA_LEN (1 + 1 + 2 + REMOTEID_SERVICE_DATA_LEN)
#define REMOTEID_ADV_INTERVAL_100_MS 0x00a0

#ifndef CONFIG_REMOTEID_BLE_TX_INTERVAL_MS
#define CONFIG_REMOTEID_BLE_TX_INTERVAL_MS 250
#endif

static const char *TAG = "remoteid_ble";

static const remoteid_message_index_t s_message_schedule[] = {
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_OPERATOR_ID,
    REMOTEID_MESSAGE_LOCATION,
};

static remoteid_message_bundle_t s_bundle;
static uint8_t s_message_counters[ODID_MSG_COUNTER_AMOUNT];
static size_t s_schedule_index;
static uint8_t s_own_addr_type;

static esp_power_level_t configured_ble_tx_power(void)
{
#if CONFIG_REMOTEID_BLE_TX_POWER_N12
    return ESP_PWR_LVL_N12;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N9
    return ESP_PWR_LVL_N9;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N6
    return ESP_PWR_LVL_N6;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N3
    return ESP_PWR_LVL_N3;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N0
    return ESP_PWR_LVL_N0;
#elif CONFIG_REMOTEID_BLE_TX_POWER_P3
    return ESP_PWR_LVL_P3;
#elif CONFIG_REMOTEID_BLE_TX_POWER_P6
    return ESP_PWR_LVL_P6;
#else
    return ESP_PWR_LVL_P9;
#endif
}

static esp_err_t configure_ble_tx_power(void)
{
    esp_power_level_t power = configured_ble_tx_power();

    ESP_RETURN_ON_ERROR(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, power), TAG, "set default BLE TX power");
    ESP_RETURN_ON_ERROR(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, power), TAG, "set advertising BLE TX power");
    ESP_LOGI(TAG, "BLE advertising TX power configured to level %d", power);

    return ESP_OK;
}

static esp_err_t advertise_message(const uint8_t message[ODID_MESSAGE_SIZE])
{
    uint8_t adv_data[REMOTEID_LEGACY_ADV_DATA_LEN] = {0};
    struct ble_gap_adv_params adv_params = {0};
    uint8_t message_type = message[0] >> 4;
    uint8_t counter_index = message_type < ODID_MSG_COUNTER_AMOUNT ? message_type : ODID_MSG_COUNTER_PACKED;

    adv_data[0] = REMOTEID_LEGACY_ADV_DATA_LEN - 1;
    adv_data[1] = BLE_HS_ADV_TYPE_SVC_DATA_UUID16;
    adv_data[2] = REMOTEID_SERVICE_UUID & 0xff;
    adv_data[3] = (REMOTEID_SERVICE_UUID >> 8) & 0xff;
    adv_data[4] = REMOTEID_AD_APPLICATION_CODE;
    adv_data[5] = s_message_counters[counter_index];
    memcpy(&adv_data[6], message, ODID_MESSAGE_SIZE);

    ble_gap_adv_stop();

    int rc = ble_gap_adv_set_data(adv_data, sizeof(adv_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set BLE advertising data: %d", rc);
        return ESP_FAIL;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_NON;
    adv_params.channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP;
    adv_params.itvl_min = REMOTEID_ADV_INTERVAL_100_MS;
    adv_params.itvl_max = REMOTEID_ADV_INTERVAL_100_MS;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start BLE advertising: %d", rc);
        return ESP_FAIL;
    }

    s_message_counters[counter_index]++;
    remoteid_led_pulse();
    return ESP_OK;
}

static void remoteid_ble_task(void *arg)
{
    (void)arg;
    remoteid_state_t snapshot;

    ESP_LOGI(TAG, "BLE transport waiting for Remote ID Basic ID and Operator ID");
    ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
    ESP_LOGI(TAG, "BLE transport identity ready, starting advertisements");

    while (true) {
        if (!remoteid_store_is_ready()) {
            ble_gap_adv_stop();
            ESP_LOGW(TAG, "BLE transport paused until Remote ID identity is ready again");
            ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
        }

        ESP_ERROR_CHECK(remoteid_store_get_snapshot(&snapshot));
        esp_err_t static_rc = remoteid_encode_static_messages(&snapshot, &s_bundle);
        if (static_rc != ESP_OK) {
            ESP_LOGE(TAG, "failed to encode static messages");
            vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_BLE_TX_INTERVAL_MS));
            continue;
        }

        remoteid_message_index_t message_index = s_message_schedule[s_schedule_index];
        esp_err_t rc = remoteid_encode_dynamic_message(&snapshot, &s_bundle, message_index);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "failed to encode dynamic message %u, broadcasting stale message", message_index);
        }

        const uint8_t *message = s_bundle.messages[message_index];
        if (advertise_message(message) == ESP_OK) {
            ESP_LOGD(TAG, "advertising ODID message type %u", message[0] >> 4);
        }

        s_schedule_index = (s_schedule_index + 1) % (sizeof(s_message_schedule) / sizeof(s_message_schedule[0]));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_BLE_TX_INTERVAL_MS));
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer BLE address type: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE ready, starting OpenDroneID advertiser");
    if (xTaskCreate(remoteid_ble_task, "remoteid_ble", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create remoteid_ble task");
    }
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t remoteid_ble_start(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "initialize NimBLE");
    ESP_RETURN_ON_ERROR(configure_ble_tx_power(), TAG, "configure BLE TX power");

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(nimble_host_task);

    return ESP_OK;
}
