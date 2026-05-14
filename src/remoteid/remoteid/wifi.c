#include "wifi.h"

#include <stdint.h>
#include <string.h>

#include "encode.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "opendroneid.h"
#include "sdkconfig.h"
#include "store.h"

#define REMOTEID_WIFI_FRAME_BUF_SIZE 512

#ifndef CONFIG_REMOTEID_WIFI_BEACON_SSID
#define CONFIG_REMOTEID_WIFI_BEACON_SSID "OpenDroneID"
#endif

#ifndef CONFIG_REMOTEID_WIFI_BEACON_CHANNEL
#define CONFIG_REMOTEID_WIFI_BEACON_CHANNEL 6
#endif

#ifndef CONFIG_REMOTEID_WIFI_CHANNEL
#define CONFIG_REMOTEID_WIFI_CHANNEL CONFIG_REMOTEID_WIFI_BEACON_CHANNEL
#endif

#ifndef CONFIG_REMOTEID_WIFI_TX_POWER_DBM
#define CONFIG_REMOTEID_WIFI_TX_POWER_DBM 20
#endif

#ifndef CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS
#define CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS 1000
#endif

#ifndef CONFIG_REMOTEID_WIFI_NAN_TX_INTERVAL_MS
#define CONFIG_REMOTEID_WIFI_NAN_TX_INTERVAL_MS 1000
#endif

#ifndef CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
#define CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON 0
#endif

#ifndef CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
#define CONFIG_REMOTEID_TRANSPORT_WIFI_NAN 0
#endif

static const char *TAG = "remoteid_wifi";

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
static uint8_t s_beacon_send_counter;
#endif
#if CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
static uint8_t s_nan_send_counter;
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
static uint16_t beacon_interval_tu(void)
{
    uint32_t interval_us = CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS * 1000U;
    uint32_t interval_tu = interval_us / 1024U;

    return (uint16_t)(interval_tu > 0 ? interval_tu : 1);
}
#endif

static void log_transmitted_message_types(const char *transport, const ODID_UAS_Data *uas_data)
{
    for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++) {
        if (uas_data->BasicIDValid[i]) {
            ESP_LOGD(TAG, "%s advertising ODID message type %u (Basic ID slot %d)", transport,
                     ODID_MESSAGETYPE_BASIC_ID, i);
        }
    }

    if (uas_data->LocationValid) {
        ESP_LOGD(TAG, "%s advertising ODID message type %u (Location)", transport, ODID_MESSAGETYPE_LOCATION);
    }

    for (int i = 0; i < ODID_AUTH_MAX_PAGES; i++) {
        if (uas_data->AuthValid[i]) {
            ESP_LOGD(TAG, "%s advertising ODID message type %u (Auth page %d)", transport, ODID_MESSAGETYPE_AUTH, i);
        }
    }

    if (uas_data->SelfIDValid) {
        ESP_LOGD(TAG, "%s advertising ODID message type %u (Self ID)", transport, ODID_MESSAGETYPE_SELF_ID);
    }

    if (uas_data->SystemValid) {
        ESP_LOGD(TAG, "%s advertising ODID message type %u (System)", transport, ODID_MESSAGETYPE_SYSTEM);
    }

    if (uas_data->OperatorIDValid) {
        ESP_LOGD(TAG, "%s advertising ODID message type %u (Operator ID)", transport, ODID_MESSAGETYPE_OPERATOR_ID);
    }
}

static wifi_mode_t wifi_mode(void)
{
#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON && CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
    return WIFI_MODE_APSTA;
#elif CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
    return WIFI_MODE_AP;
#else
    return WIFI_MODE_STA;
#endif
}

static esp_err_t init_wifi_driver(void)
{
    esp_err_t rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return rc;
    }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return rc;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(wifi_mode()), TAG, "set Wi-Fi mode");

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, CONFIG_REMOTEID_WIFI_BEACON_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(CONFIG_REMOTEID_WIFI_BEACON_SSID);
    ap_config.ap.channel = CONFIG_REMOTEID_WIFI_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 0;
    ap_config.ap.beacon_interval = CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "configure Wi-Fi AP");
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
#if !CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(CONFIG_REMOTEID_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "set Wi-Fi channel");
#endif
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(CONFIG_REMOTEID_WIFI_TX_POWER_DBM * 4), TAG, "set Wi-Fi TX power");

    ESP_LOGI(TAG,
             "Wi-Fi Remote ID enabled on channel %d at %d dBm (beacon=%d interval=%d ms, nan=%d interval=%d ms)",
             CONFIG_REMOTEID_WIFI_CHANNEL, CONFIG_REMOTEID_WIFI_TX_POWER_DBM, CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON,
             CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS, CONFIG_REMOTEID_TRANSPORT_WIFI_NAN,
             CONFIG_REMOTEID_WIFI_NAN_TX_INTERVAL_MS);
    return ESP_OK;
}

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
static esp_err_t send_remoteid_beacon(void)
{
    remoteid_state_t snapshot;
    ODID_UAS_Data uas_data;
    uint8_t frame[REMOTEID_WIFI_FRAME_BUF_SIZE];
    uint8_t mac[6];

    ESP_RETURN_ON_ERROR(remoteid_store_get_snapshot(&snapshot), TAG, "get Remote ID snapshot");
    ESP_RETURN_ON_ERROR(remoteid_encode_uas_data(&snapshot, &uas_data), TAG, "build OpenDroneID UAS data");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "get Wi-Fi AP MAC");

    int len = odid_wifi_build_message_pack_beacon_frame(
        &uas_data, (const char *)mac, CONFIG_REMOTEID_WIFI_BEACON_SSID, strlen(CONFIG_REMOTEID_WIFI_BEACON_SSID),
        beacon_interval_tu(), s_beacon_send_counter, frame, sizeof(frame));
    if (len < 0) {
        ESP_LOGE(TAG, "failed to build OpenDroneID Wi-Fi Beacon frame: %d", len);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_80211_tx(WIFI_IF_AP, frame, len, true), TAG, "transmit Wi-Fi Beacon frame");
    s_beacon_send_counter++;
    log_transmitted_message_types("Wi-Fi Beacon", &uas_data);
    remoteid_led_pulse();

    return ESP_OK;
}
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
static esp_err_t send_remoteid_nan(void)
{
    remoteid_state_t snapshot;
    ODID_UAS_Data uas_data;
    uint8_t frame[REMOTEID_WIFI_FRAME_BUF_SIZE];
    uint8_t mac[6];
    int len;

    ESP_RETURN_ON_ERROR(remoteid_store_get_snapshot(&snapshot), TAG, "get Remote ID snapshot");
    ESP_RETURN_ON_ERROR(remoteid_encode_uas_data(&snapshot, &uas_data), TAG, "build OpenDroneID UAS data");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "get Wi-Fi STA MAC");

    len = odid_wifi_build_nan_sync_beacon_frame((const char *)mac, frame, sizeof(frame));
    if (len < 0) {
        ESP_LOGE(TAG, "failed to build OpenDroneID Wi-Fi NAN sync frame: %d", len);
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true), TAG, "transmit Wi-Fi NAN sync frame");

    len = odid_wifi_build_message_pack_nan_action_frame(&uas_data, (const char *)mac, s_nan_send_counter, frame,
                                                        sizeof(frame));
    if (len < 0) {
        ESP_LOGE(TAG, "failed to build OpenDroneID Wi-Fi NAN action frame: %d", len);
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true), TAG, "transmit Wi-Fi NAN action frame");
    s_nan_send_counter++;
    log_transmitted_message_types("Wi-Fi NAN", &uas_data);
    remoteid_led_pulse();

    return ESP_OK;
}
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
static void remoteid_wifi_beacon_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Wi-Fi Beacon transport waiting for Remote ID Basic ID and Operator ID");
    ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
    ESP_LOGI(TAG, "Wi-Fi Beacon transport identity ready, starting transmissions");

    while (true) {
        if (!remoteid_store_is_ready()) {
            ESP_LOGW(TAG, "Wi-Fi Beacon transport paused until Remote ID identity is ready again");
            ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
        }

        if (send_remoteid_beacon() == ESP_OK) {
            ESP_LOGD(TAG, "transmitted OpenDroneID Wi-Fi Beacon");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_WIFI_BEACON_TX_INTERVAL_MS));
    }
}
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
static void remoteid_wifi_nan_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Wi-Fi NAN transport waiting for Remote ID Basic ID and Operator ID");
    ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
    ESP_LOGI(TAG, "Wi-Fi NAN transport identity ready, starting transmissions");

    while (true) {
        if (!remoteid_store_is_ready()) {
            ESP_LOGW(TAG, "Wi-Fi NAN transport paused until Remote ID identity is ready again");
            ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
        }

        if (send_remoteid_nan() == ESP_OK) {
            ESP_LOGD(TAG, "transmitted OpenDroneID Wi-Fi NAN");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_WIFI_NAN_TX_INTERVAL_MS));
    }
}
#endif

esp_err_t remoteid_wifi_start(void)
{
    ESP_RETURN_ON_ERROR(init_wifi_driver(), TAG, "initialize Wi-Fi Remote ID transport");

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON
    if (xTaskCreate(remoteid_wifi_beacon_task, "remoteid_wifi_bcn", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create remoteid_wifi_bcn task");
        return ESP_ERR_NO_MEM;
    }
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
    if (xTaskCreate(remoteid_wifi_nan_task, "remoteid_wifi_nan", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create remoteid_wifi_nan task");
        return ESP_ERR_NO_MEM;
    }
#endif

    return ESP_OK;
}
