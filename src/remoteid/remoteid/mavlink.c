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

#define REMOTEID_MAVLINK_BUF_SIZE 256
#define REMOTEID_MAVLINK_QUEUE_SIZE 0

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
    float altitude_m = msg.altitude_geodetic;
    if (altitude_m <= -999.0f) {
        altitude_m = msg.altitude_barometric;
    }

    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_LOCATION,
        .data.location.has_position = has_position,
        .data.location.latitude = (double)msg.latitude / 10000000.0,
        .data.location.longitude = (double)msg.longitude / 10000000.0,
        .data.location.altitude_m = altitude_m,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted MAVLink Location (has_position=%d)", has_position);
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Location update: state queue full");
    }
}

static void handle_message(const mavlink_message_t *message)
{
    switch (message->msgid) {
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID:
        submit_basic_id(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID:
        submit_operator_id(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION:
        submit_location(message);
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
                    NULL, 7, NULL) != pdPASS) {
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
