#include "mavlink.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_MAVLINK_INPUT

#include <math.h>
#include <string.h>

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
#include "input.h"
#include "store.h"

#define REMOTEID_MAVLINK_BUF_SIZE      256
// Driver ring buffers: room for several MESSAGE_PACK frames while the task is busy.
#define REMOTEID_MAVLINK_RING_SIZE     2048
#define REMOTEID_MAVLINK_QUEUE_SIZE    0
// Read returns after this long without new bytes, so a frame is parsed as soon
// as it has arrived instead of waiting for a full read buffer.
#define REMOTEID_MAVLINK_READ_TIMEOUT_MS 10
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

// The autopilot this module belongs to: bound to the first HEARTBEAT from an
// autopilot component (MAV_COMP_ID_AUTOPILOT1) of the configured target system,
// or of any system when none is configured, and never re-bound. Arm state and
// UTC are only taken from it. 0 until bound; single-byte access is atomic.
static volatile uint8_t s_autopilot_sysid;

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

// Own-aircraft data only. id_or_mac is set when a message describes another UA
// picked up by a Remote ID receiver, and the ODID transmitter/receiver
// components (236-238) are such receivers. Target 0/0 alone does not make a
// message about this aircraft.
static bool from_own_aircraft(const mavlink_message_t *message, const uint8_t *id_or_mac)
{
    if (message->compid >= MAV_COMP_ID_ODID_TXRX_1 && message->compid <= MAV_COMP_ID_ODID_TXRX_3) {
        return false;
    }
    return !id_or_mac || !remoteid_input_describes_other_aircraft(id_or_mac);
}

// id_or_mac is NULL for messages without that field (SYSTEM_UPDATE).
static bool accept_odid(const mavlink_message_t *message, uint8_t target_system, uint8_t target_component,
                        const uint8_t *id_or_mac, const char *name)
{
    if (!target_matches(target_system, target_component)) {
        ESP_LOGD(TAG, "ignored %s for target system=%u component=%u", name, target_system, target_component);
        return false;
    }
    if (!from_own_aircraft(message, id_or_mac)) {
        ESP_LOGD(TAG, "ignored %s describing another aircraft (from %u/%u)", name, message->sysid, message->compid);
        return false;
    }
    return true;
}

static bool from_bound_autopilot(const mavlink_message_t *message)
{
    return s_autopilot_sysid != 0 && message->sysid == s_autopilot_sysid &&
           message->compid == MAV_COMP_ID_AUTOPILOT1;
}

static void submit_heartbeat(const mavlink_message_t *message)
{
    mavlink_heartbeat_t msg = {0};
    mavlink_msg_heartbeat_decode(message, &msg);

    // Only the flight controller itself: GCS, cameras, other components and
    // Remote ID modules report MAV_AUTOPILOT_INVALID or another component ID.
    if (msg.autopilot == MAV_AUTOPILOT_INVALID || message->compid != MAV_COMP_ID_AUTOPILOT1) {
        return;
    }
    if (CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM != 0 &&
        message->sysid != CONFIG_REMOTEID_MAVLINK_TARGET_SYSTEM) {
        return;
    }
    if (s_autopilot_sysid == 0) {
        s_autopilot_sysid = message->sysid;
        ESP_LOGI(TAG, "bound to autopilot system %u", message->sysid);
    } else if (message->sysid != s_autopilot_sysid) {
        ESP_LOGD(TAG, "ignored HEARTBEAT from autopilot system %u (bound to %u)",
                 message->sysid, s_autopilot_sysid);
        return;
    }

    // The store captures the take-off point on a disarmed-to-armed edge. Sent on
    // change, and again on the next HEARTBEAT if the queue was full.
    static int s_reported_armed = -1;
    bool is_armed = (msg.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
    if (is_armed != s_reported_armed) {
        remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_ARMED, .data.armed.armed = is_armed };
        if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
            s_reported_armed = is_armed;
            ESP_LOGI(TAG, "vehicle %s", is_armed ? "armed" : "disarmed");
        } else {
            ESP_LOGW(TAG, "arm state update dropped: state queue full, retrying");
        }
    }

#if CONFIG_REMOTEID_MAVLINK_EMERGENCY_FROM_HEARTBEAT
    // Derive the OpenDroneID emergency status from the autopilot's HEARTBEAT
    // system_status. Pushed only on change so the store status tracks the vehicle.
    static bool s_emergency_state = false;
    bool emergency = (msg.system_status == MAV_STATE_EMERGENCY ||
                      msg.system_status == MAV_STATE_FLIGHT_TERMINATION
#if CONFIG_REMOTEID_MAVLINK_EMERGENCY_INCLUDE_CRITICAL
                      || msg.system_status == MAV_STATE_CRITICAL
#endif
                      );
    if (emergency != s_emergency_state) {
        remoteid_store_update_t update = {
            .type = REMOTEID_STORE_UPDATE_EMERGENCY,
            .data.emergency.active = emergency,
        };
        if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
            s_emergency_state = emergency;
            ESP_LOGW(TAG, "autopilot emergency %s (system_status=%u)",
                     emergency ? "ASSERTED" : "cleared", msg.system_status);
        }
    }
#endif
}

static void submit_basic_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_basic_id_t msg = { 0 };
    mavlink_msg_open_drone_id_basic_id_decode(message, &msg);
    if (accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "Basic ID")) {
        remoteid_input_basic_id(&msg, "MAVLink");
    }
}

static void submit_operator_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_operator_id_t msg = { 0 };
    mavlink_msg_open_drone_id_operator_id_decode(message, &msg);
    if (accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "Operator ID")) {
        remoteid_input_operator_id(&msg, "MAVLink");
    }
}

// EU 2019/945 requires that the user cannot modify the aircraft position data.
// In strict mode it is therefore taken only from the bound autopilot, not from a
// ground station or companion computer on the same link.
static bool location_source_allowed(const mavlink_message_t *message)
{
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    if (!from_bound_autopilot(message)) {
        ESP_LOGD(TAG, "ignored Location from %u/%u: strict mode takes it only from the autopilot",
                 message->sysid, message->compid);
        return false;
    }
#endif
    (void)message;
    return true;
}

static void submit_location(const mavlink_message_t *message)
{
    mavlink_open_drone_id_location_t msg = { 0 };
    mavlink_msg_open_drone_id_location_decode(message, &msg);
    if (accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "Location") &&
        location_source_allowed(message)) {
        remoteid_input_location(&msg, "MAVLink");
    }
}

static void submit_system(const mavlink_message_t *message)
{
    mavlink_open_drone_id_system_t msg = { 0 };
    mavlink_msg_open_drone_id_system_decode(message, &msg);
    if (accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "System")) {
        remoteid_input_system(&msg, "MAVLink");
    }
}

static void submit_system_update(const mavlink_message_t *message)
{
    mavlink_open_drone_id_system_update_t msg = { 0 };
    mavlink_msg_open_drone_id_system_update_decode(message, &msg);

    if (!accept_odid(message, msg.target_system, msg.target_component, NULL, "System Update")) {
        return;
    }

    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_SYSTEM_UPDATE,
        .data.system_update.has_operator_position    = msg.operator_latitude != 0 || msg.operator_longitude != 0,
        .data.system_update.operator_latitude        = (double)msg.operator_latitude  / 10000000.0,
        .data.system_update.operator_longitude       = (double)msg.operator_longitude / 10000000.0,
        .data.system_update.operator_altitude_geo_m  = msg.operator_altitude_geo <= (float)INV_ALT ? (float)INV_ALT : msg.operator_altitude_geo,
        .data.system_update.timestamp                = msg.timestamp,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "dropped MAVLink System Update: state queue full");
    }
}

static void submit_self_id(const mavlink_message_t *message)
{
    mavlink_open_drone_id_self_id_t msg = { 0 };
    mavlink_msg_open_drone_id_self_id_decode(message, &msg);
    if (accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "Self ID")) {
        remoteid_input_self_id(&msg, "MAVLink");
    }
}

#if CONFIG_REMOTEID_AUTH_RELAY

static void submit_auth_page(const ODID_Auth_data *page)
{
    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_AUTH, .data.auth.page = *page };
    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted Authentication page %u", page->DataPage);
    } else {
        ESP_LOGW(TAG, "dropped Authentication page %u: state queue full", page->DataPage);
    }
}

static void submit_authentication(const mavlink_message_t *message)
{
    mavlink_open_drone_id_authentication_t msg = { 0 };
    mavlink_msg_open_drone_id_authentication_decode(message, &msg);

    if (!accept_odid(message, msg.target_system, msg.target_component, msg.id_or_mac, "Authentication")) {
        return;
    }

    ODID_Auth_data page;
    odid_initAuthData(&page);
    page.DataPage = msg.data_page;
    page.AuthType = (ODID_authtype_t)msg.authentication_type;
    if (msg.data_page == 0) {
        page.LastPageIndex = msg.last_page_index;
        page.Length        = msg.length;
        page.Timestamp     = msg.timestamp;
        // Page 0 carries only the first 17 bytes; the rest of the MAVLink field is padding.
        memcpy(page.AuthData, msg.authentication_data, ODID_AUTH_PAGE_ZERO_DATA_SIZE);
    } else {
        memcpy(page.AuthData, msg.authentication_data, ODID_AUTH_PAGE_NONZERO_DATA_SIZE);
    }
    // The store rejects pages beyond ODID_AUTH_MAX_PAGES.
    submit_auth_page(&page);
}

#endif /* CONFIG_REMOTEID_AUTH_RELAY */

// A Message Pack carries already-encoded OpenDroneID (ASTM) messages, not nested
// MAVLink messages, so each entry is decoded with the OpenDroneID core decoders
// and translated into a store update. Encoded values are already physical units,
// matching what the encoder writes back out, so no MAVLink-style scaling applies.
static void apply_odid_encoded_message(const mavlink_message_t *message, const uint8_t raw[ODID_MESSAGE_SIZE])
{
    uint8_t type = raw[0] >> 4;
    remoteid_store_update_t update = { 0 };

    switch (type) {
    case ODID_MESSAGETYPE_BASIC_ID: {
        ODID_BasicID_encoded enc;
        ODID_BasicID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeBasicIDMessage(&data, &enc) == ODID_SUCCESS) {
            mavlink_open_drone_id_basic_id_t msg = { .id_type = data.IDType, .ua_type = data.UAType };
            memcpy(msg.uas_id, data.UASID, ODID_ID_SIZE);
            remoteid_input_basic_id(&msg, "MAVLink pack");
        }
        return;
    }
    case ODID_MESSAGETYPE_LOCATION: {
        ODID_Location_encoded enc;
        ODID_Location_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (!location_source_allowed(message) || decodeLocationMessage(&data, &enc) != ODID_SUCCESS) {
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
        update.data.system.timestamp               = data.Timestamp;
        break;
    }
    case ODID_MESSAGETYPE_OPERATOR_ID: {
        ODID_OperatorID_encoded enc;
        ODID_OperatorID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeOperatorIDMessage(&data, &enc) == ODID_SUCCESS) {
            mavlink_open_drone_id_operator_id_t msg = { .operator_id_type = data.OperatorIdType };
            memcpy(msg.operator_id, data.OperatorId, sizeof(msg.operator_id));
            remoteid_input_operator_id(&msg, "MAVLink pack");
        }
        return;
    }
    case ODID_MESSAGETYPE_SELF_ID: {
        ODID_SelfID_encoded enc;
        ODID_SelfID_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeSelfIDMessage(&data, &enc) == ODID_SUCCESS) {
            mavlink_open_drone_id_self_id_t msg = { .description_type = data.DescType };
            memcpy(msg.description, data.Desc, sizeof(msg.description));
            remoteid_input_self_id(&msg, "MAVLink pack");
        }
        return;
    }
#if CONFIG_REMOTEID_AUTH_RELAY
    case ODID_MESSAGETYPE_AUTH: {
        ODID_Auth_encoded enc;
        ODID_Auth_data data;
        memcpy(&enc, raw, sizeof(enc));
        if (decodeAuthMessage(&data, &enc) == ODID_SUCCESS) {
            submit_auth_page(&data);
        }
        return;
    }
#endif
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

    if (!accept_odid(message, pack.target_system, pack.target_component, pack.id_or_mac, "Message Pack")) {
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
        apply_odid_encoded_message(message, &pack.messages[i * ODID_MESSAGE_SIZE]);
    }
}

// Seed the shared UTC clock from the bound autopilot's SYSTEM_TIME so every
// OpenDroneID timestamp reflects real time. A GCS or companion also sends
// SYSTEM_TIME, from a clock that may be wrong, so only the autopilot counts.
static void capture_system_time(const mavlink_message_t *message)
{
    if (!from_bound_autopilot(message)) {
        return;
    }
    mavlink_system_time_t msg = { 0 };
    mavlink_msg_system_time_decode(message, &msg);
    remoteid_clock_set_utc_us(msg.time_unix_usec);
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
#if CONFIG_REMOTEID_AUTH_RELAY
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_AUTHENTICATION:
        submit_authentication(message);
        break;
#endif
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
        .rx_buffer_size = REMOTEID_MAVLINK_RING_SIZE,
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

    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_REMOTEID_MAVLINK_UART_NUM, REMOTEID_MAVLINK_RING_SIZE,
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
        // The pilot position (or its take-off fallback) is part of the shared
        // health, so MAVLink and DroneCAN report the same readiness.
        bool ready = (health == REMOTEID_HEALTH_OK);
        const char *fail = remoteid_store_health_text(health);
        mavlink_message_t msg;


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

// Keep a fixed operator location fresh on the autopilot: republish it at a
// steady cadence so it survives the GCS disconnecting. A live location is
// GCS-owned and never republished (the store expires it). Neither is a take-off
// point that replaced an expired live location: republishing it would come back
// through the autopilot as a fresh "live" position.
static void remoteid_mavlink_operator_location_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_PERIOD_MS);
    const uint8_t    compid = (uint8_t)CONFIG_REMOTEID_MAVLINK_OWN_COMPONENT_ID;

    while (true) {
        remoteid_state_t snap;
        double latitude, longitude;
        float altitude_geo_m;
        ODID_operator_location_type_t type;
        if (remoteid_store_get_snapshot(&snap) == ESP_OK && !snap.operator_position_expired &&
            remoteid_operator_position(&snap, &latitude, &longitude, &altitude_geo_m, &type) &&
            type != ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS) {
            if (remoteid_clock_is_synced()) {
                // Fixed/takeoff location: keep it fresh toward the autopilot.
                int32_t lat = (int32_t)lround(latitude  * 1e7);
                int32_t lon = (int32_t)lround(longitude * 1e7);
                mavlink_message_t msg;
                mavlink_msg_open_drone_id_system_update_pack_chan(
                    outgoing_system_id(), compid, MAVLINK_COMM_1, &msg,
                    0, 0, lat, lon, altitude_geo_m, remoteid_clock_odid_2019());
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
        int len = mavlink_transport_read(buf, sizeof(buf), pdMS_TO_TICKS(REMOTEID_MAVLINK_READ_TIMEOUT_MS));
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
    ESP_LOGI(TAG, "MAVLink operator-location loop enabled (%d ms republish)",
             CONFIG_REMOTEID_MAVLINK_OPERATOR_LOCATION_PERIOD_MS);
#endif

    return ESP_OK;
}

#else

esp_err_t remoteid_mavlink_start(void)
{
    return ESP_OK;
}

#endif
