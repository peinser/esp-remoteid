#include "mavlink.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_MAVLINK_INPUT

#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#include "common/mavlink.h"
#pragma GCC diagnostic pop
#include "driver/uart.h"
#if CONFIG_REMOTEID_MAVLINK_TRANSPORT_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
#include "clock.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "store.h"

#define REMOTEID_MAVLINK_BUF_SIZE      256
#define REMOTEID_MAVLINK_QUEUE_SIZE    0
#define REMOTEID_MAVLINK_TASK_PRIORITY 5

// True when the firmware transmits MAVLink back to the autopilot (status output
// and/or the operator-location loop); both share the TX prerequisite and the
// outgoing identity/transport helpers below.
#if CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT || CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP
#define REMOTEID_MAVLINK_OUTPUT 1
#else
#define REMOTEID_MAVLINK_OUTPUT 0
#endif

static const char *TAG = "remoteid_mavlink";

#if REMOTEID_MAVLINK_OUTPUT
// System ID adopted from the flight controller's HEARTBEAT; 0 until one is seen.
// Single-byte access is atomic on this target, so no lock is needed.
static volatile uint8_t s_autopilot_sysid;
#endif

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
    size_t len = strnlen((const char *)src, src_len);
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
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

#if REMOTEID_MAVLINK_OUTPUT
    // Adopt the flight controller's system ID so our outgoing messages appear as
    // a component of the same vehicle.
    s_autopilot_sysid = message->sysid;
#endif

    static bool s_was_armed = false;
    bool is_armed = (msg.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;

    if (is_armed && !s_was_armed) {
        remoteid_store_update_t update = {.type = REMOTEID_STORE_UPDATE_TAKEOFF};
        if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
            ESP_LOGI(TAG, "vehicle armed, capturing takeoff position");
        } else {
            ESP_LOGW(TAG, "vehicle armed, takeoff position capture dropped: queue full");
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
        // MAVLink speeds are cm/s (unknown: 25500 / 6300); ODID wants m/s, so scale by 100.
        .data.location.speed_horizontal = msg.speed_horizontal >= 25500
                                          ? (float)INV_SPEED_H
                                          : msg.speed_horizontal / 100.0f,
        .data.location.speed_vertical   = msg.speed_vertical   >= 6300
                                          ? (float)INV_SPEED_V
                                          : msg.speed_vertical   / 100.0f,
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

static void submit_system_update(const mavlink_message_t *message)
{
    mavlink_open_drone_id_system_update_t msg = { 0 };
    mavlink_msg_open_drone_id_system_update_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored System Update for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_SYSTEM_UPDATE,
        .data.system_update.has_operator_position    = msg.operator_latitude != 0 || msg.operator_longitude != 0,
        .data.system_update.operator_latitude        = (double)msg.operator_latitude  / 10000000.0,
        .data.system_update.operator_longitude       = (double)msg.operator_longitude / 10000000.0,
        .data.system_update.operator_altitude_geo_m  = msg.operator_altitude_geo,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "dropped MAVLink System Update: state queue full");
    }
}

static void submit_self_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_self_id_t msg = { 0 };
    mavlink_msg_open_drone_id_self_id_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored Self ID for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    remoteid_store_update_t update = {
        .type                   = REMOTEID_STORE_UPDATE_SELF_ID,
        .data.self_id.desc_type = (ODID_desctype_t)msg.description_type,
    };
    copy_mavlink_text(update.data.self_id.desc, sizeof(update.data.self_id.desc),
                      msg.description, sizeof(msg.description));

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGI(TAG, "accepted MAVLink Self ID");
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Self ID update: state queue full");
    }
}

static void submit_authentication(const mavlink_message_t *message)
{
    mavlink_open_drone_id_authentication_t msg = { 0 };
    mavlink_msg_open_drone_id_authentication_decode(message, &msg);

    if (!target_matches(msg.target_system, msg.target_component)) {
        ESP_LOGD(TAG, "ignored Authentication for target system=%u component=%u", msg.target_system, msg.target_component);
        return;
    }

    if (msg.data_page >= ODID_AUTH_MAX_PAGES) {
        ESP_LOGW(TAG, "ignored Authentication page %u beyond max %d", msg.data_page, ODID_AUTH_MAX_PAGES);
        return;
    }

    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_AUTH };
    ODID_Auth_data *page = &update.data.auth.page;
    odid_initAuthData(page);
    page->DataPage = msg.data_page;
    page->AuthType = (ODID_authtype_t)msg.authentication_type;
    if (msg.data_page == 0) {
        page->LastPageIndex = msg.last_page_index;
        page->Length        = msg.length;
        page->Timestamp     = msg.timestamp;
        // Page 0 carries only the first 17 bytes; the rest of the MAVLink field is padding.
        memcpy(page->AuthData, msg.authentication_data, ODID_AUTH_PAGE_ZERO_DATA_SIZE);
    } else {
        memcpy(page->AuthData, msg.authentication_data, ODID_AUTH_PAGE_NONZERO_DATA_SIZE);
    }

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGI(TAG, "accepted MAVLink Authentication page %u", msg.data_page);
    } else {
        ESP_LOGW(TAG, "dropped MAVLink Authentication update: state queue full");
    }
}

// A Message Pack carries already-encoded OpenDroneID (ASTM) messages, not nested
// MAVLink messages, so each entry is decoded with the OpenDroneID core decoders
// and translated into a store update. Encoded values are already physical units,
// matching what the encoder writes back out, so no MAVLink-style scaling applies.
static void apply_odid_encoded_message(const uint8_t raw[ODID_MESSAGE_SIZE])
{
    uint8_t type = raw[0] >> 4;
    remoteid_store_update_t update = { 0 };

    switch (type) {
    case ODID_MESSAGETYPE_BASIC_ID: {
        ODID_BasicID_encoded enc;
        ODID_BasicID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeBasicIDMessage(&data, &enc) != ODID_SUCCESS) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_BASIC_ID;
        update.data.basic_id.id_type = data.IDType;
        update.data.basic_id.ua_type = data.UAType;
        copy_mavlink_text(update.data.basic_id.uas_id, sizeof(update.data.basic_id.uas_id),
                          data.UASID, sizeof(data.UASID));
        break;
    }
    case ODID_MESSAGETYPE_LOCATION: {
        ODID_Location_encoded enc;
        ODID_Location_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeLocationMessage(&data, &enc) != ODID_SUCCESS) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_LOCATION;
        update.data.location.has_position     = data.Latitude != 0 || data.Longitude != 0;
        update.data.location.latitude         = data.Latitude;
        update.data.location.longitude        = data.Longitude;
        update.data.location.altitude_geo_m   = data.AltitudeGeo;
        update.data.location.altitude_baro_m  = data.AltitudeBaro;
        update.data.location.status           = data.Status;
        update.data.location.speed_horizontal = data.SpeedHorizontal;
        update.data.location.speed_vertical   = data.SpeedVertical;
        update.data.location.direction        = data.Direction;
        update.data.location.height           = data.Height;
        update.data.location.height_type      = data.HeightType;
        update.data.location.horiz_acc        = data.HorizAccuracy;
        update.data.location.vert_acc         = data.VertAccuracy;
        update.data.location.baro_acc         = data.BaroAccuracy;
        update.data.location.speed_acc        = data.SpeedAccuracy;
        update.data.location.ts_acc           = data.TSAccuracy;
        update.data.location.timestamp        = data.TimeStamp;
        break;
    }
    case ODID_MESSAGETYPE_SYSTEM: {
        ODID_System_encoded enc;
        ODID_System_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeSystemMessage(&data, &enc) != ODID_SUCCESS) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_SYSTEM;
        update.data.system.has_operator_position   = data.OperatorLatitude != 0 || data.OperatorLongitude != 0;
        update.data.system.operator_latitude       = data.OperatorLatitude;
        update.data.system.operator_longitude      = data.OperatorLongitude;
        update.data.system.operator_altitude_geo_m = data.OperatorAltitudeGeo;
        update.data.system.operator_location_type  = data.OperatorLocationType;
        update.data.system.area_count              = data.AreaCount;
        update.data.system.area_radius             = data.AreaRadius;
        update.data.system.area_ceiling_m          = data.AreaCeiling;
        update.data.system.area_floor_m            = data.AreaFloor;
        update.data.system.classification_type     = data.ClassificationType;
        update.data.system.eu_category             = data.CategoryEU;
        update.data.system.eu_class                = data.ClassEU;
        break;
    }
    case ODID_MESSAGETYPE_OPERATOR_ID: {
        ODID_OperatorID_encoded enc;
        ODID_OperatorID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeOperatorIDMessage(&data, &enc) != ODID_SUCCESS) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_OPERATOR_ID;
        copy_mavlink_text(update.data.operator_id.operator_id, sizeof(update.data.operator_id.operator_id),
                          data.OperatorId, sizeof(data.OperatorId));
        break;
    }
    case ODID_MESSAGETYPE_SELF_ID: {
        ODID_SelfID_encoded enc;
        ODID_SelfID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeSelfIDMessage(&data, &enc) != ODID_SUCCESS) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_SELF_ID;
        update.data.self_id.desc_type = data.DescType;
        copy_mavlink_text(update.data.self_id.desc, sizeof(update.data.self_id.desc),
                          data.Desc, sizeof(data.Desc));
        break;
    }
    case ODID_MESSAGETYPE_AUTH: {
        ODID_Auth_encoded enc;
        ODID_Auth_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeAuthMessage(&data, &enc) != ODID_SUCCESS || data.DataPage >= ODID_AUTH_MAX_PAGES) {
            return;
        }
        update.type = REMOTEID_STORE_UPDATE_AUTH;
        update.data.auth.page = data;
        break;
    }
    default:
        return;
    }

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "dropped packed ODID message type %u: state queue full", type);
    }
}

static void handle_message_pack(const mavlink_message_t *message)
{
    mavlink_open_drone_id_message_pack_t pack = { 0 };
    mavlink_msg_open_drone_id_message_pack_decode(message, &pack);

    if (!target_matches(pack.target_system, pack.target_component)) {
        ESP_LOGD(TAG, "ignored Message Pack for target system=%u component=%u",
                 pack.target_system, pack.target_component);
        return;
    }

    if (pack.single_message_size != ODID_MESSAGE_SIZE) {
        ESP_LOGW(TAG, "ignored Message Pack with single message size %u (expected %d)",
                 pack.single_message_size, ODID_MESSAGE_SIZE);
        return;
    }

    uint8_t max_messages = sizeof(pack.messages) / ODID_MESSAGE_SIZE;
    if (pack.msg_pack_size == 0 || pack.msg_pack_size > max_messages) {
        ESP_LOGW(TAG, "ignored Message Pack with message count %u (max %u)",
                 pack.msg_pack_size, max_messages);
        return;
    }

    for (uint8_t i = 0; i < pack.msg_pack_size; i++) {
        apply_odid_encoded_message(&pack.messages[i * ODID_MESSAGE_SIZE]);
    }
}

// Seed the shared UTC clock from the flight controller's SYSTEM_TIME so every
// OpenDroneID timestamp reflects real time. Available whenever MAVLink input is
// enabled, independent of the operator-location loop.
static void capture_system_time(const mavlink_message_t *message)
{
    mavlink_system_time_t msg = { 0 };
    mavlink_msg_system_time_decode(message, &msg);
    remoteid_clock_set_utc(msg.time_unix_usec / 1000000ULL);
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
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM_UPDATE:
        submit_system_update(message);
        break;
    case MAVLINK_MSG_ID_SYSTEM_TIME:
        capture_system_time(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID:
        submit_self_id(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_AUTHENTICATION:
        submit_authentication(message);
        break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_MESSAGE_PACK:
        handle_message_pack(message);
        break;
    default:
        break;
    }
}

// ---- Transport (UART or native USB Serial/JTAG) ----------------------------

#if CONFIG_REMOTEID_MAVLINK_TRANSPORT_USB_SERIAL_JTAG

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "Set the console secondary output to None (CONFIG_ESP_CONSOLE_SECONDARY_NONE) when using USB Serial/JTAG for MAVLink, or log output will corrupt the MAVLink stream."
#endif

#define MAVLINK_TRANSPORT_LABEL "USB Serial/JTAG"

static esp_err_t mavlink_transport_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = REMOTEID_MAVLINK_BUF_SIZE * 2,
        .tx_buffer_size = REMOTEID_MAVLINK_BUF_SIZE * 2,
    };
    return usb_serial_jtag_driver_install(&cfg);
}

static int mavlink_transport_read(uint8_t *buf, size_t len, TickType_t ticks)
{
    return usb_serial_jtag_read_bytes(buf, len, ticks);
}

#if REMOTEID_MAVLINK_OUTPUT
static void mavlink_transport_write(const uint8_t *buf, size_t len)
{
    usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
}
#endif

#else /* UART */

#define MAVLINK_TRANSPORT_LABEL "UART"

static esp_err_t mavlink_transport_init(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_MAVLINK_RX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "MAVLink RX GPIO must be configured for the UART transport");

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_REMOTEID_MAVLINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int tx_pin = CONFIG_REMOTEID_MAVLINK_TX_GPIO >= 0 ? CONFIG_REMOTEID_MAVLINK_TX_GPIO : UART_PIN_NO_CHANGE;

    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_REMOTEID_MAVLINK_UART_NUM, REMOTEID_MAVLINK_BUF_SIZE * 2,
                                            0, REMOTEID_MAVLINK_QUEUE_SIZE, NULL, 0),
                        TAG, "install MAVLink UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_REMOTEID_MAVLINK_UART_NUM, &uart_config),
                        TAG, "configure MAVLink UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_REMOTEID_MAVLINK_UART_NUM, tx_pin, CONFIG_REMOTEID_MAVLINK_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "configure MAVLink UART pins");
    return ESP_OK;
}

static int mavlink_transport_read(uint8_t *buf, size_t len, TickType_t ticks)
{
    return uart_read_bytes(CONFIG_REMOTEID_MAVLINK_UART_NUM, buf, len, ticks);
}

#if REMOTEID_MAVLINK_OUTPUT
static void mavlink_transport_write(const uint8_t *buf, size_t len)
{
    uart_write_bytes(CONFIG_REMOTEID_MAVLINK_UART_NUM, buf, len);
}
#endif

#endif /* transport */

#if REMOTEID_MAVLINK_OUTPUT

static uint8_t outgoing_system_id(void)
{
    if (CONFIG_REMOTEID_MAVLINK_OWN_SYSTEM_ID != 0) {
        return (uint8_t)CONFIG_REMOTEID_MAVLINK_OWN_SYSTEM_ID;
    }
    uint8_t adopted = s_autopilot_sysid;
    return adopted != 0 ? adopted : 1;
}

static void send_mavlink(const mavlink_message_t *message)
{
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    uint16_t len = mavlink_msg_to_send_buffer(buf, message);
    mavlink_transport_write(buf, len);
}

#endif /* REMOTEID_MAVLINK_OUTPUT */

#if CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT

// Report HEARTBEAT and OPEN_DRONE_ID_ARM_STATUS to the flight controller. The
// arm status mirrors the broadcast readiness gate: GOOD_TO_ARM once a usable
// identity is present (and the transports are broadcasting), otherwise a
// pre-arm failure so a configured autopilot blocks arming.
static void remoteid_mavlink_status_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_REMOTEID_MAVLINK_STATUS_RATE_HZ);
    const uint8_t compid = (uint8_t)CONFIG_REMOTEID_MAVLINK_OWN_COMPONENT_ID;

    while (true) {
        uint8_t sysid = outgoing_system_id();
        remoteid_health_t health = remoteid_store_health();
        bool ready = (health == REMOTEID_HEALTH_OK);
        const char *fail;
        switch (health) {
        case REMOTEID_HEALTH_SELFTEST_FAILED:     fail = "Remote ID self-test failed"; break;
        case REMOTEID_HEALTH_INVALID_SERIAL:      fail = "Invalid UAS serial (CTA-2063-A)"; break;
        case REMOTEID_HEALTH_WAITING_OPERATOR_ID: fail = "Operator ID not set"; break;
        case REMOTEID_HEALTH_WAITING_BASIC_ID:    fail = "UAS ID not set"; break;
        default:                                  fail = "Remote ID not ready"; break;
        }
        mavlink_message_t msg;

#if CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP
        // When the firmware owns operator-location compliance, a missing or
        // expired operator location must also block arming.
        if (ready) {
            remoteid_state_t snap;
            if (remoteid_store_get_snapshot(&snap) == ESP_OK && !snap.has_operator_position) {
                ready = false;
                fail = "Operator location missing or stale";
            }
        }
#endif

        mavlink_msg_heartbeat_pack_chan(sysid, compid, MAVLINK_COMM_1, &msg,
                                        MAV_TYPE_ODID, MAV_AUTOPILOT_INVALID, 0, 0,
                                        ready ? MAV_STATE_ACTIVE : MAV_STATE_STANDBY);
        send_mavlink(&msg);

        // The error field is a fixed char[50] copied in full by the packer (it
        // does not stop at the NUL), so use a zero-padded buffer to avoid sending
        // adjacent flash bytes. All-zero padding also lets v2 truncate trailing
        // zeros, keeping the frame small.
        char arm_error[50] = {0};
        if (!ready) {
            strlcpy(arm_error, fail, sizeof(arm_error));
        }
        mavlink_msg_open_drone_id_arm_status_pack_chan(
            sysid, compid, MAVLINK_COMM_1, &msg,
            ready ? MAV_ODID_ARM_STATUS_GOOD_TO_ARM : MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC,
            arm_error);
        send_mavlink(&msg);

        vTaskDelay(period);
    }
}

#endif /* CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT */

#if CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP

// Keep the operator location compliant locally: republish a fixed operator
// location toward the autopilot at a steady cadence (so it survives the GCS
// disconnecting), and expire a live operator location when its GCS stream stops.
static void remoteid_mavlink_operator_location_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_PERIOD_MS);
    const uint8_t    compid = (uint8_t)CONFIG_REMOTEID_MAVLINK_OWN_COMPONENT_ID;

    while (true) {
        remoteid_state_t snap;
        if (remoteid_store_get_snapshot(&snap) == ESP_OK && snap.has_operator_position) {
            if (snap.operator_location_type == ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS) {
                // Live location is GCS-owned; never republish, only expire it.
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                uint32_t age_ms = now_ms - snap.operator_position_updated_ms;
                if (age_ms > CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_EXPIRY_MS) {
                    remoteid_store_update_t expire = { .type = REMOTEID_STORE_UPDATE_OPERATOR_LOCATION_EXPIRE };
                    if (remoteid_store_submit(&expire, pdMS_TO_TICKS(100)) == ESP_OK) {
                        ESP_LOGW(TAG, "live operator location stale (%u ms), expiring", (unsigned)age_ms);
                    }
                }
            } else if (remoteid_clock_is_synced()) {
                // Fixed/takeoff location: keep it fresh toward the autopilot.
                int32_t lat = (int32_t)(snap.operator_latitude  * 1e7);
                int32_t lon = (int32_t)(snap.operator_longitude * 1e7);
                mavlink_message_t msg;
                mavlink_msg_open_drone_id_system_update_pack_chan(
                    outgoing_system_id(), compid, MAVLINK_COMM_1, &msg,
                    0, 0, lat, lon, snap.operator_altitude_geo_m, remoteid_clock_odid_2019());
                send_mavlink(&msg);
                ESP_LOGD(TAG, "republished fixed operator location via System Update");
            } else {
                ESP_LOGD(TAG, "operator-location loop waiting for SYSTEM_TIME seed");
            }
        }
        vTaskDelay(period);
    }
}

#endif /* CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP */

static void remoteid_mavlink_task(void *arg)
{
    (void)arg;
    uint8_t buf[REMOTEID_MAVLINK_BUF_SIZE];
    mavlink_message_t message;
    mavlink_status_t status;

    memset(&message, 0, sizeof(message));
    memset(&status, 0, sizeof(status));

    while (true) {
        int len = mavlink_transport_read(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        for (int i = 0; i < len; i++) {
            if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &message, &status)) {
                handle_message(&message);
            }
        }
    }
}

esp_err_t remoteid_mavlink_start(void)
{
#if REMOTEID_MAVLINK_OUTPUT && CONFIG_REMOTEID_MAVLINK_TRANSPORT_UART
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_MAVLINK_TX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "MAVLink TX GPIO must be configured for MAVLink output (status / operator-location)");
#endif

    ESP_RETURN_ON_ERROR(mavlink_transport_init(), TAG, "initialize MAVLink transport");

    if (xTaskCreate(remoteid_mavlink_task, "remoteid_mavlink", CONFIG_REMOTEID_MAVLINK_TASK_STACK,
                    NULL, REMOTEID_MAVLINK_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create MAVLink input task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MAVLink OpenDroneID input started over %s", MAVLINK_TRANSPORT_LABEL);

#if CONFIG_REMOTEID_MAVLINK_STATUS_OUTPUT
    if (xTaskCreate(remoteid_mavlink_status_task, "remoteid_mav_tx", 4096,
                    NULL, REMOTEID_MAVLINK_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create MAVLink status output task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MAVLink status output enabled (HEARTBEAT + ARM_STATUS) at %d Hz on component %d",
             CONFIG_REMOTEID_MAVLINK_STATUS_RATE_HZ, CONFIG_REMOTEID_MAVLINK_OWN_COMPONENT_ID);
#endif

#if CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_LOOP
    if (xTaskCreate(remoteid_mavlink_operator_location_task, "remoteid_mav_oploc", 4096,
                    NULL, REMOTEID_MAVLINK_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create MAVLink operator-location loop task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MAVLink operator-location loop enabled (%d ms republish, %d ms expiry)",
             CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_PERIOD_MS,
             CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_EXPIRY_MS);
#endif

    return ESP_OK;
}

#else

esp_err_t remoteid_mavlink_start(void)
{
    return ESP_OK;
}

#endif
