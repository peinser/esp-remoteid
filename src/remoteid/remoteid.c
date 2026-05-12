#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "opendroneid.h"
#include "sdkconfig.h"

#define ODID_SERVICE_UUID 0xFFFA
#define ODID_AD_APPLICATION_CODE 0x0D
#define ODID_SERVICE_DATA_LEN (2 + ODID_MESSAGE_SIZE)
#define ODID_LEGACY_ADV_DATA_LEN (1 + 1 + 2 + ODID_SERVICE_DATA_LEN)
#define ODID_ADV_INTERVAL_100_MS 0x00a0
#define ODID_ADV_ROTATION_MS 250

static const char *TAG = "esp_remoteid";

#if CONFIG_REMOTEID_HAS_POSITION
static const double DRONE_LATITUDE  = (double)CONFIG_REMOTEID_TAKEOFF_LAT_1E6 / 1e6;
static const double DRONE_LONGITUDE = (double)CONFIG_REMOTEID_TAKEOFF_LON_1E6 / 1e6;
static const float  DRONE_ALTITUDE_M = (float)CONFIG_REMOTEID_TAKEOFF_ALT_M;
#endif

typedef enum remoteid_message_index {
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_OPERATOR_ID,
    REMOTEID_MESSAGE_COUNT,
} remoteid_message_index_t;

static uint8_t s_messages[REMOTEID_MESSAGE_COUNT][ODID_MESSAGE_SIZE];
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
static uint8_t s_message_counters[ODID_MSG_COUNTER_AMOUNT];
static size_t s_schedule_index;
static uint8_t s_own_addr_type;

static esp_err_t check_odid_encode(int rc, const char *message_name)
{
    if (rc == ODID_SUCCESS) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "failed to encode %s OpenDroneID message", message_name);
    return ESP_FAIL;
}

static esp_err_t build_basic_id(uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_BasicID_data data;
    ODID_BasicID_encoded encoded;

    odid_initBasicIDData(&data);

#if CONFIG_REMOTEID_ID_TYPE_SERIAL_NUMBER
    data.IDType = ODID_IDTYPE_SERIAL_NUMBER;
#else
    data.IDType = ODID_IDTYPE_CAA_REGISTRATION_ID;
#endif

#if CONFIG_REMOTEID_UA_TYPE_AEROPLANE
    data.UAType = ODID_UATYPE_AEROPLANE;
#elif CONFIG_REMOTEID_UA_TYPE_HYBRID_LIFT
    data.UAType = ODID_UATYPE_HYBRID_LIFT;
#elif CONFIG_REMOTEID_UA_TYPE_OTHER
    data.UAType = ODID_UATYPE_OTHER;
#else
    data.UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
#endif

    strncpy(data.UASID, CONFIG_REMOTEID_UAS_ID, sizeof(data.UASID) - 1);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeBasicIDMessage(&encoded, &data), "Basic ID"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

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

static esp_err_t build_location(uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_Location_data data;
    ODID_Location_encoded encoded;

    odid_initLocationData(&data);

#if CONFIG_REMOTEID_HAS_POSITION
    data.Status = ODID_STATUS_AIRBORNE;
    data.Direction = 0.0f;
    data.SpeedHorizontal = 0.0f;
    data.SpeedVertical = 0.0f;
    data.Latitude = DRONE_LATITUDE;
    data.Longitude = DRONE_LONGITUDE;
    data.AltitudeBaro = DRONE_ALTITUDE_M;
    data.AltitudeGeo = DRONE_ALTITUDE_M;
    data.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
    data.Height = 0.0f;
    data.HorizAccuracy = ODID_HOR_ACC_UNKNOWN;
    data.VertAccuracy = ODID_VER_ACC_UNKNOWN;
    data.BaroAccuracy = ODID_VER_ACC_UNKNOWN;
    data.SpeedAccuracy = ODID_SPEED_ACC_UNKNOWN;
    data.TSAccuracy = ODID_TIME_ACC_UNKNOWN;
    data.TimeStamp = (float)(uptime_seconds() % MAX_TIMESTAMP);
#else
    /* No position available: broadcast ASTM F3411 invalid sentinel values. */
    data.Status = ODID_STATUS_UNDECLARED;
    data.Direction = (float)INV_DIR;
    data.SpeedHorizontal = (float)INV_SPEED_H;
    data.SpeedVertical = (float)INV_SPEED_V;
    data.Latitude = 0.0;
    data.Longitude = 0.0;
    data.AltitudeBaro = (float)INV_ALT;
    data.AltitudeGeo = (float)INV_ALT;
    data.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
    data.Height = (float)INV_ALT;
    data.HorizAccuracy = ODID_HOR_ACC_UNKNOWN;
    data.VertAccuracy = ODID_VER_ACC_UNKNOWN;
    data.BaroAccuracy = ODID_VER_ACC_UNKNOWN;
    data.SpeedAccuracy = ODID_SPEED_ACC_UNKNOWN;
    data.TSAccuracy = ODID_TIME_ACC_UNKNOWN;
    data.TimeStamp = (float)INV_TIMESTAMP;
#endif

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeLocationMessage(&encoded, &data), "Location"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_system(uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_System_data data;
    ODID_System_encoded encoded;

    odid_initSystemData(&data);
    data.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    data.ClassificationType = ODID_CLASSIFICATION_TYPE_EU;

#if CONFIG_REMOTEID_HAS_POSITION
    data.OperatorLatitude = DRONE_LATITUDE;
    data.OperatorLongitude = DRONE_LONGITUDE;
    data.OperatorAltitudeGeo = DRONE_ALTITUDE_M;
#else
    data.OperatorLatitude = 0.0;
    data.OperatorLongitude = 0.0;
    data.OperatorAltitudeGeo = (float)INV_ALT;
#endif

    data.AreaCount = 1;
    data.AreaRadius = 0;
    data.AreaCeiling = -1000.0f;
    data.AreaFloor = -1000.0f;

#if CONFIG_REMOTEID_EU_CATEGORY_UNDECLARED
    data.CategoryEU = ODID_CATEGORY_EU_UNDECLARED;
#elif CONFIG_REMOTEID_EU_CATEGORY_SPECIFIC
    data.CategoryEU = ODID_CATEGORY_EU_SPECIFIC;
#elif CONFIG_REMOTEID_EU_CATEGORY_CERTIFIED
    data.CategoryEU = ODID_CATEGORY_EU_CERTIFIED;
#else
    data.CategoryEU = ODID_CATEGORY_EU_OPEN;
#endif

#if CONFIG_REMOTEID_EU_CLASS_C0
    data.ClassEU = ODID_CLASS_EU_CLASS_0;
#elif CONFIG_REMOTEID_EU_CLASS_C1
    data.ClassEU = ODID_CLASS_EU_CLASS_1;
#elif CONFIG_REMOTEID_EU_CLASS_C2
    data.ClassEU = ODID_CLASS_EU_CLASS_2;
#elif CONFIG_REMOTEID_EU_CLASS_C3
    data.ClassEU = ODID_CLASS_EU_CLASS_3;
#elif CONFIG_REMOTEID_EU_CLASS_C4
    data.ClassEU = ODID_CLASS_EU_CLASS_4;
#elif CONFIG_REMOTEID_EU_CLASS_C5
    data.ClassEU = ODID_CLASS_EU_CLASS_5;
#elif CONFIG_REMOTEID_EU_CLASS_C6
    data.ClassEU = ODID_CLASS_EU_CLASS_6;
#else
    data.ClassEU = ODID_CLASS_EU_UNDECLARED;
#endif

    data.Timestamp = uptime_seconds();

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeSystemMessage(&encoded, &data), "System"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_operator_id(uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_OperatorID_data data;
    ODID_OperatorID_encoded encoded;

    odid_initOperatorIDData(&data);
    data.OperatorIdType = ODID_OPERATOR_ID;
    strncpy(data.OperatorId, CONFIG_REMOTEID_OPERATOR_ID, sizeof(data.OperatorId) - 1);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeOperatorIDMessage(&encoded, &data), "Operator ID"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_messages(void)
{
    ESP_RETURN_ON_ERROR(build_basic_id(s_messages[REMOTEID_MESSAGE_BASIC_ID]), TAG, "");
    ESP_RETURN_ON_ERROR(build_operator_id(s_messages[REMOTEID_MESSAGE_OPERATOR_ID]), TAG, "");

    return ESP_OK;
}

static esp_err_t advertise_message(const uint8_t message[ODID_MESSAGE_SIZE])
{
    uint8_t adv_data[ODID_LEGACY_ADV_DATA_LEN] = {0};
    struct ble_gap_adv_params adv_params = {0};
    uint8_t message_type = message[0] >> 4;
    uint8_t counter_index = message_type < ODID_MSG_COUNTER_AMOUNT ? message_type : ODID_MSG_COUNTER_PACKED;

    adv_data[0] = ODID_LEGACY_ADV_DATA_LEN - 1;
    adv_data[1] = BLE_HS_ADV_TYPE_SVC_DATA_UUID16;
    adv_data[2] = ODID_SERVICE_UUID & 0xff;
    adv_data[3] = (ODID_SERVICE_UUID >> 8) & 0xff;
    adv_data[4] = ODID_AD_APPLICATION_CODE;
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
    adv_params.itvl_min = ODID_ADV_INTERVAL_100_MS;
    adv_params.itvl_max = ODID_ADV_INTERVAL_100_MS;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start BLE advertising: %d", rc);
        return ESP_FAIL;
    }

    s_message_counters[counter_index]++;
    return ESP_OK;
}

static void remoteid_task(void *arg)
{
    (void)arg;

    while (true) {
        remoteid_message_index_t message_index = s_message_schedule[s_schedule_index];
        if (message_index == REMOTEID_MESSAGE_LOCATION) {
            if (build_location(s_messages[REMOTEID_MESSAGE_LOCATION]) != ESP_OK) {
                ESP_LOGE(TAG, "Location encode failed, broadcasting stale message");
            }
        } else if (message_index == REMOTEID_MESSAGE_SYSTEM) {
            if (build_system(s_messages[REMOTEID_MESSAGE_SYSTEM]) != ESP_OK) {
                ESP_LOGE(TAG, "System encode failed, broadcasting stale message");
            }
        }

        const uint8_t *message = s_messages[message_index];
        if (advertise_message(message) == ESP_OK) {
            ESP_LOGD(TAG, "advertising ODID message type %u", message[0] >> 4);
        }
        s_schedule_index = (s_schedule_index + 1) % (sizeof(s_message_schedule) / sizeof(s_message_schedule[0]));
        vTaskDelay(pdMS_TO_TICKS(ODID_ADV_ROTATION_MS));
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
    if (xTaskCreate(remoteid_task, "remoteid_ble", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create remoteid_task");
    }
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(build_messages());

    ESP_ERROR_CHECK(nimble_port_init());
    ESP_ERROR_CHECK(configure_ble_tx_power());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(nimble_host_task);
}
