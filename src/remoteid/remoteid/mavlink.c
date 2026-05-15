#include "mavlink.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_MAVLINK_INPUT

#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#include "common/mavlink.h"
#pragma GCC diagnostic pop
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "store.h"

#define REMOTEID_MAVLINK_BUF_SIZE      256
#define REMOTEID_MAVLINK_QUEUE_SIZE    0
#define REMOTEID_MAVLINK_TASK_PRIORITY 5

static const char *TAG = "remoteid_mavlink";

static bool target_matches(uint8_t target_system, uint8_t target_component)
{
    if (CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM != 0 &&
        target_system != 0 &&
        target_system != CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM) {
        return false;
    }

    if (CONFIG_REMOTEID_MAVLINK_TARGET_COMPONENT != 0 &&
        target_component != 0 &&
        target_component != CONFIG_REMOTEID_MAVLINK_TARGET_COMPONENT) {
        return false;
    }

    return true;
}

static void copy_mavlink_text(char *dst, size_t dst_len, const void *src, size_t src_len)
{
    size_t len = 0;
    const char *text = (const char *)src;

    while (len < src_len && text[len] != '\0') {
        len++;
    }

    if (len >= dst_len) {
        len = dst_len - 1;
    }

    memcpy(dst, text, len);
    dst[len] = '\0';
}

static void submit_heartbeat(const mavlink_message_t *message)
{
    mavlink_heartbeat_t msg = {0};
    mavlink_msg_heartbeat_decode(message, &msg);

    // Only track arm state from autopilots; ignore GCS, cameras, and other components.
    if (msg.autopilot == MAV_AUTOPILOT_INVALID) {
        return;
    }

    // Filter by configured source system ID if set (HEARTBEAT has no target fields;
    // match against the sender's system ID in the MAVLink header instead).
    if (CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM != 0 &&
        message->sysid != CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM) {
        return;
    }

    static bool s_was_armed = false;
    bool is_armed = (msg.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;

    if (is_armed && !s_was_armed) {
        remoteid_store_update_t update = {.type = REMOTEID_STORE_UPDATE_TAKEOFF};
        if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
            ESP_LOGI(TAG, "vehicle armed — capturing takeoff position");
        } else {
            ESP_LOGW(TAG, "vehicle armed — takeoff position capture dropped: queue full");
        }
    }

    s_was_armed = is_armed;
}

static void submit_basic_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_basic_id_t msg = { 0 };
    mavlink_msg_open_drone_id_basic_id_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored Basic ID for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_BASIC_ID,
        .data.basic_id.id_type = (ODID_idtype_t)msg.id_type,
        .data.basic_id.ua_type = (ODID_uatype_t)msg.ua_type,
    };
    copy_mavlink_text(update.data.basic_id.uas_id, sizeof(update.data.basic_id.uas_id),
                      msg.uas_id, sizeof(msg.uas_id));

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGI(TAG, "accepted MAVLink Basic ID");
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Basic ID update: state queue full");
    }
}

static void submit_operator_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_operator_id_t msg = { 0 };
    mavlink_msg_open_drone_id_operator_id_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored Operator ID for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_OPERATOR_ID,
    };
    copy_mavlink_text(update.data.operator_id.operator_id, sizeof(update.data.operator_id.operator_id),
                      msg.operator_id, sizeof(msg.operator_id));

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGI(TAG, "accepted MAVLink Operator ID");
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Operator ID update: state queue full");
    }
}

static void submit_location(const mavlink_message_t *message)
{
    mavlink_open_drone_id_location_t msg = { 0 };
    mavlink_msg_open_drone_id_location_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored Location for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    bool has_position = msg.latitude != 0 || msg.longitude != 0;

    remoteid_store_update_t update = {
        .type                           = REMOTEID_STORE_UPDATE_LOCATION,
        .data.location.has_position     = has_position,
        .data.location.latitude         = (double)msg.latitude  / 10000000.0,
        .data.location.longitude        = (double)msg.longitude / 10000000.0,
        .data.location.altitude_geo_m   = msg.altitude_geodetic  <= (float)INV_ALT ? (float)INV_ALT : msg.altitude_geodetic,
        .data.location.altitude_baro_m  = msg.altitude_barometric <= (float)INV_ALT ? (float)INV_ALT : msg.altitude_barometric,
        .data.location.status           = (ODID_status_t)msg.status,
        .data.location.speed_horizontal = msg.speed_horizontal >= (float)INV_SPEED_H ? (float)INV_SPEED_H : msg.speed_horizontal,
        .data.location.speed_vertical   = msg.speed_vertical   >= (float)INV_SPEED_V ? (float)INV_SPEED_V : msg.speed_vertical,
        .data.location.direction        = msg.direction >= 36100
                                          ? (float)INV_DIR
                                          : msg.direction / 100.0f,
        .data.location.height           = msg.height <= (float)INV_ALT ? (float)INV_ALT : msg.height,
        .data.location.height_type      = (ODID_Height_reference_t)msg.height_reference,
        .data.location.horiz_acc        = (ODID_Horizontal_accuracy_t)msg.horizontal_accuracy,
        .data.location.vert_acc         = (ODID_Vertical_accuracy_t)msg.vertical_accuracy,
        .data.location.baro_acc         = (ODID_Vertical_accuracy_t)msg.barometer_accuracy,
        .data.location.speed_acc        = (ODID_Speed_accuracy_t)msg.speed_accuracy,
        .data.location.ts_acc           = (ODID_Timestamp_accuracy_t)msg.timestamp_accuracy,
        .data.location.timestamp        = msg.timestamp >= (float)MAX_TIMESTAMP ? -1.0f : msg.timestamp,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted MAVLink Location (has_position=%d status=%u)", has_position, msg.status);
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Location update: state queue full");
    }
}

static void submit_system(const mavlink_message_t *message)
{
    mavlink_open_drone_id_system_t msg = { 0 };
    mavlink_msg_open_drone_id_system_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored System for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    bool has_operator_position = msg.operator_latitude != 0 || msg.operator_longitude != 0;

    remoteid_store_update_t update = {
        .type                               = REMOTEID_STORE_UPDATE_SYSTEM,
        .data.system.has_operator_position  = has_operator_position,
        .data.system.operator_latitude      = (double)msg.operator_latitude  / 10000000.0,
        .data.system.operator_longitude     = (double)msg.operator_longitude / 10000000.0,
        .data.system.operator_altitude_geo_m = msg.operator_altitude_geo <= (float)INV_ALT ? (float)INV_ALT : msg.operator_altitude_geo,
        .data.system.operator_location_type = (ODID_operator_location_type_t)msg.operator_location_type,
        .data.system.area_count             = msg.area_count,
        .data.system.area_radius            = msg.area_radius,
        .data.system.area_ceiling_m         = msg.area_ceiling,
        .data.system.area_floor_m           = msg.area_floor,
        .data.system.classification_type    = (ODID_classification_type_t)msg.classification_type,
        .data.system.eu_category            = (ODID_category_EU_t)msg.category_eu,
        .data.system.eu_class               = (ODID_class_EU_t)msg.class_eu,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted MAVLink System (has_operator_position=%d)", has_operator_position);
    } else {
        ESP_LOGW(TAG, "dropped MAVLink System update: state queue full");
    }
}

static void handle_message(const mavlink_message_t *message)
{
    switch (message->msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
        submit_heartbeat(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID:
        submit_basic_id(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID:
        submit_operator_id(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION:
        submit_location(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM:
        submit_system(message);
        break;
    default:
        break;
    }
}

static void remoteid_mavlink_task(void *arg)
{
    (void)arg;
    uint8_t buf[REMOTEID_MAVLINK_BUF_SIZE];
    mavlink_message_t message;
    mavlink_status_t status;

    memset(&message, 0, sizeof(message));
    memset(&status, 0, sizeof(status));

    while (true) {
        int len = uart_read_bytes(CONFIG_REMOTEID_MAVLINK_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        for (int i = 0; i < len; i++) {
            if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &message, &status)) {
                handle_message(&message);
            }
        }
    }
}

esp_err_t remoteid_mavlink_start(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_MAVLINK_RX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "MAVLink RX GPIO must be configured when MAVLink input is enabled");

    uart_config_t uart_config = {
        .baud_rate = CONFIG_REMOTEID_MAVLINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    int tx_pin = CONFIG_REMOTEID_MAVLINK_TX_GPIO >= 0 ? CONFIG_REMOTEID_MAVLINK_TX_GPIO : UART_PIN_NO_CHANGE;
    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_REMOTEID_MAVLINK_UART_NUM,
                                            REMOTEID_MAVLINK_BUF_SIZE * 2,
                                            0,
                                            REMOTEID_MAVLINK_QUEUE_SIZE,
                                            NULL,
                                            0),
                        TAG, "install MAVLink UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_REMOTEID_MAVLINK_UART_NUM, &uart_config),
                        TAG, "configure MAVLink UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_REMOTEID_MAVLINK_UART_NUM,
                                     tx_pin,
                                     CONFIG_REMOTEID_MAVLINK_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "configure MAVLink UART pins");

    if (xTaskCreate(remoteid_mavlink_task, "remoteid_mavlink", CONFIG_REMOTEID_MAVLINK_TASK_STACK,
                    NULL, REMOTEID_MAVLINK_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create MAVLink input task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MAVLink OpenDroneID input started on UART%d at %d baud",
             CONFIG_REMOTEID_MAVLINK_UART_NUM, CONFIG_REMOTEID_MAVLINK_BAUD);
    return ESP_OK;
}

#else

esp_err_t remoteid_mavlink_start(void)
{
    return ESP_OK;
}

#endif
