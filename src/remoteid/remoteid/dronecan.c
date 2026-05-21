#include "dronecan.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_DRONECAN_INPUT

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "store.h"

#define REMOTEID_DRONECAN_TASK_PRIORITY 5
#define REMOTEID_DRONECAN_EXT_ID_MASK   0x1FFFFFFFUL
#define REMOTEID_DRONECAN_NODE_ID_MASK  0x7FU
#define REMOTEID_DRONECAN_SERVICE_BIT   BIT(7)
#define REMOTEID_DRONECAN_TAIL_START    BIT(7)
#define REMOTEID_DRONECAN_TAIL_END      BIT(6)
#define REMOTEID_DRONECAN_TAIL_TOGGLE   BIT(5)
#define REMOTEID_DRONECAN_TAIL_TRANSFER_ID_MASK 0x1FU
#define REMOTEID_DRONECAN_MAX_TRANSFER_PAYLOAD 256
#define REMOTEID_DRONECAN_TYPE_NODE_STATUS 341
#define REMOTEID_DRONECAN_TYPE_GNSS_FIX 1060
#define REMOTEID_DRONECAN_TYPE_GNSS_FIX2 1063
#define REMOTEID_DRONECAN_FIX2_MIN_BITS 394
#define REMOTEID_DRONECAN_REMOTEID_IDENTITY_SIZE 42
#define REMOTEID_DRONECAN_INVALID_DIRECTION 361.0f
#define REMOTEID_DRONECAN_PI 3.14159265358979323846f

static const char *TAG = "remoteid_dronecan";

typedef struct dronecan_transfer_meta {
    uint8_t priority;
    uint16_t data_type_id;
    uint8_t source_node_id;
    bool service_not_message;
    bool start_of_transfer;
    bool end_of_transfer;
    bool toggle;
    uint8_t transfer_id;
} dronecan_transfer_meta_t;

typedef struct dronecan_rx_transfer {
    bool active;
    dronecan_transfer_meta_t meta;
    uint8_t payload[REMOTEID_DRONECAN_MAX_TRANSFER_PAYLOAD];
    size_t payload_len;
    bool expected_toggle;
} dronecan_rx_transfer_t;

typedef struct dronecan_bit_reader {
    const uint8_t *data;
    size_t bit_len;
} dronecan_bit_reader_t;

static dronecan_rx_transfer_t s_rx;

static twai_timing_config_t dronecan_timing_config(void)
{
#if CONFIG_REMOTEID_DRONECAN_BITRATE_125000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
#elif CONFIG_REMOTEID_DRONECAN_BITRATE_250000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
#elif CONFIG_REMOTEID_DRONECAN_BITRATE_500000
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
#else
    return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
#endif
}

static const char *known_message_name(uint16_t data_type_id)
{
    switch (data_type_id) {
    case REMOTEID_DRONECAN_TYPE_NODE_STATUS:
        return "uavcan.protocol.NodeStatus";
    case REMOTEID_DRONECAN_TYPE_GNSS_FIX:
        return "uavcan.equipment.gnss.Fix";
    case REMOTEID_DRONECAN_TYPE_GNSS_FIX2:
        return "uavcan.equipment.gnss.Fix2";
    default:
        if (data_type_id == CONFIG_REMOTEID_DRONECAN_REMOTEID_IDENTITY_DTID) {
            return "com.peinser.remoteid.Identity";
        }
        return NULL;
    }
}

static bool decode_transfer_meta(const twai_message_t *frame, dronecan_transfer_meta_t *meta)
{
    if (!frame->extd || frame->rtr || frame->data_length_code == 0) {
        return false;
    }

    uint32_t can_id = frame->identifier & REMOTEID_DRONECAN_EXT_ID_MASK;
    meta->priority = (uint8_t)((can_id >> 24) & 0x1FU);
    meta->data_type_id = (uint16_t)((can_id >> 8) & 0xFFFFU);
    meta->service_not_message = (can_id & REMOTEID_DRONECAN_SERVICE_BIT) != 0;
    meta->source_node_id = (uint8_t)(can_id & REMOTEID_DRONECAN_NODE_ID_MASK);

    uint8_t tail = frame->data[frame->data_length_code - 1];
    meta->start_of_transfer = (tail & REMOTEID_DRONECAN_TAIL_START) != 0;
    meta->end_of_transfer = (tail & REMOTEID_DRONECAN_TAIL_END) != 0;
    meta->toggle = (tail & REMOTEID_DRONECAN_TAIL_TOGGLE) != 0;
    meta->transfer_id = tail & REMOTEID_DRONECAN_TAIL_TRANSFER_ID_MASK;

    return true;
}

static bool supported_message_type(uint16_t data_type_id)
{
    return data_type_id == REMOTEID_DRONECAN_TYPE_GNSS_FIX2 ||
           data_type_id == CONFIG_REMOTEID_DRONECAN_REMOTEID_IDENTITY_DTID;
}

static uint64_t read_u64(const dronecan_bit_reader_t *reader, size_t bit_offset, size_t bit_count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bit_count; i++) {
        size_t bit_index = bit_offset + i;
        if (bit_index >= reader->bit_len) {
            break;
        }
        if ((reader->data[bit_index / 8] & BIT(bit_index % 8)) != 0) {
            value |= UINT64_C(1) << i;
        }
    }
    return value;
}

static int64_t read_i64(const dronecan_bit_reader_t *reader, size_t bit_offset, size_t bit_count)
{
    uint64_t value = read_u64(reader, bit_offset, bit_count);
    if (bit_count == 0 || bit_count >= 64 || (value & (UINT64_C(1) << (bit_count - 1))) == 0) {
        return (int64_t)value;
    }
    return (int64_t)(value | (~UINT64_C(0) << bit_count));
}

static float read_f32(const dronecan_bit_reader_t *reader, size_t bit_offset)
{
    uint32_t raw = (uint32_t)read_u64(reader, bit_offset, 32);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static ODID_status_t dronecan_status_to_odid(uint8_t status)
{
    return status >= 2 ? ODID_STATUS_AIRBORNE : ODID_STATUS_UNDECLARED;
}

static ODID_Horizontal_accuracy_t dronecan_fix_to_hacc(uint8_t status)
{
    return status >= 2 ? ODID_HOR_ACC_10_METER : ODID_HOR_ACC_UNKNOWN;
}

static ODID_Vertical_accuracy_t dronecan_fix_to_vacc(uint8_t status)
{
    return status >= 3 ? ODID_VER_ACC_10_METER : ODID_VER_ACC_UNKNOWN;
}

static float direction_from_ned(float north_m_s, float east_m_s)
{
    if (fabsf(north_m_s) < 0.01f && fabsf(east_m_s) < 0.01f) {
        return (float)INV_DIR;
    }

    float direction = atan2f(east_m_s, north_m_s) * 180.0f / REMOTEID_DRONECAN_PI;
    if (direction < 0.0f) {
        direction += 360.0f;
    }
    if (direction >= REMOTEID_DRONECAN_INVALID_DIRECTION) {
        return (float)INV_DIR;
    }
    return direction;
}

static void submit_gnss_fix2(const uint8_t *payload, size_t payload_len, uint8_t source_node_id)
{
    dronecan_bit_reader_t reader = {
        .data = payload,
        .bit_len = payload_len * 8,
    };

    if (reader.bit_len < REMOTEID_DRONECAN_FIX2_MIN_BITS) {
        ESP_LOGW(TAG, "ignored short DroneCAN Fix2 from node=%u len=%u", source_node_id, payload_len);
        return;
    }

    int64_t lon_deg_1e8 = read_i64(&reader, 136, 37);
    int64_t lat_deg_1e8 = read_i64(&reader, 173, 37);
    int64_t height_msl_mm = read_i64(&reader, 237, 27);
    float velocity_north = read_f32(&reader, 264);
    float velocity_east = read_f32(&reader, 296);
    float velocity_down = read_f32(&reader, 328);
    uint8_t status = (uint8_t)read_u64(&reader, 366, 2);

    bool has_position = status >= 2 && (lat_deg_1e8 != 0 || lon_deg_1e8 != 0);
    float speed_horizontal = sqrtf(velocity_north * velocity_north + velocity_east * velocity_east);
    if (!isfinite(speed_horizontal) || speed_horizontal >= (float)INV_SPEED_H) {
        speed_horizontal = (float)INV_SPEED_H;
    }
    float speed_vertical = -velocity_down;
    if (!isfinite(speed_vertical) || fabsf(speed_vertical) >= (float)INV_SPEED_V) {
        speed_vertical = (float)INV_SPEED_V;
    }

    remoteid_store_update_t update = {
        .type                           = REMOTEID_STORE_UPDATE_LOCATION,
        .data.location.has_position     = has_position,
        .data.location.latitude         = (double)lat_deg_1e8 / 100000000.0,
        .data.location.longitude        = (double)lon_deg_1e8 / 100000000.0,
        .data.location.altitude_geo_m   = (float)height_msl_mm / 1000.0f,
        .data.location.altitude_baro_m  = (float)height_msl_mm / 1000.0f,
        .data.location.status           = dronecan_status_to_odid(status),
        .data.location.speed_horizontal = speed_horizontal,
        .data.location.speed_vertical   = speed_vertical,
        .data.location.direction        = direction_from_ned(velocity_north, velocity_east),
        .data.location.height           = (float)INV_ALT,
        .data.location.height_type      = ODID_HEIGHT_REF_OVER_TAKEOFF,
        .data.location.horiz_acc        = dronecan_fix_to_hacc(status),
        .data.location.vert_acc         = dronecan_fix_to_vacc(status),
        .data.location.baro_acc         = ODID_VER_ACC_UNKNOWN,
        .data.location.speed_acc        = ODID_SPEED_ACC_UNKNOWN,
        .data.location.ts_acc           = ODID_TIME_ACC_UNKNOWN,
        .data.location.timestamp        = -1.0f,
    };

    if (remoteid_store_submit(&update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted DroneCAN Fix2 node=%u has_position=%d status=%u",
                 source_node_id, has_position, status);
    } else {
        ESP_LOGW(TAG, "dropped DroneCAN Fix2 update: state queue full");
    }
}

static void copy_dronecan_text(char *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t len = 0;
    while (len < src_len && src[len] != 0) {
        len++;
    }
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void submit_remoteid_identity(const uint8_t *payload, size_t payload_len, uint8_t source_node_id)
{
    if (payload_len < REMOTEID_DRONECAN_REMOTEID_IDENTITY_SIZE) {
        ESP_LOGW(TAG, "ignored short DroneCAN Remote ID identity from node=%u len=%u",
                 source_node_id, payload_len);
        return;
    }

    remoteid_store_update_t basic_update = {
        .type = REMOTEID_STORE_UPDATE_BASIC_ID,
        .data.basic_id.id_type = (ODID_idtype_t)payload[0],
        .data.basic_id.ua_type = (ODID_uatype_t)payload[1],
    };
    copy_dronecan_text(basic_update.data.basic_id.uas_id,
                       sizeof(basic_update.data.basic_id.uas_id),
                       &payload[2], ODID_ID_SIZE);

    remoteid_store_update_t operator_update = {
        .type = REMOTEID_STORE_UPDATE_OPERATOR_ID,
    };
    copy_dronecan_text(operator_update.data.operator_id.operator_id,
                       sizeof(operator_update.data.operator_id.operator_id),
                       &payload[22], ODID_ID_SIZE);

    esp_err_t basic_err = remoteid_store_submit(&basic_update, pdMS_TO_TICKS(100));
    esp_err_t operator_err = remoteid_store_submit(&operator_update, pdMS_TO_TICKS(100));
    if (basic_err == ESP_OK && operator_err == ESP_OK) {
        ESP_LOGI(TAG, "Accepted DroneCAN Remote ID identity from node=%u", source_node_id);
    } else {
        ESP_LOGW(TAG, "Dropped DroneCAN Remote ID identity update: state queue full");
    }
}

static void handle_transfer(const dronecan_transfer_meta_t *meta, const uint8_t *payload, size_t payload_len)
{
    switch (meta->data_type_id) {
    case REMOTEID_DRONECAN_TYPE_GNSS_FIX2:
        submit_gnss_fix2(payload, payload_len, meta->source_node_id);
        break;
    default:
        if (meta->data_type_id == CONFIG_REMOTEID_DRONECAN_REMOTEID_IDENTITY_DTID) {
            submit_remoteid_identity(payload, payload_len, meta->source_node_id);
        }
        break;
    }
}

static bool append_frame_payload(dronecan_rx_transfer_t *transfer, const twai_message_t *frame)
{
    size_t frame_payload_len = frame->data_length_code - 1;
    if (transfer->payload_len + frame_payload_len > sizeof(transfer->payload)) {
        ESP_LOGW(TAG, "DroneCAN transfer too large, dropping type=%u src=%u",
                 transfer->meta.data_type_id, transfer->meta.source_node_id);
        transfer->active = false;
        return false;
    }

    memcpy(&transfer->payload[transfer->payload_len], frame->data, frame_payload_len);
    transfer->payload_len += frame_payload_len;
    return true;
}

static void handle_message_frame(const twai_message_t *frame, const dronecan_transfer_meta_t *meta)
{
    if (!supported_message_type(meta->data_type_id)) {
        return;
    }

    if (meta->start_of_transfer && meta->end_of_transfer) {
        handle_transfer(meta, frame->data, frame->data_length_code - 1);
        return;
    }

    if (meta->start_of_transfer) {
        memset(&s_rx, 0, sizeof(s_rx));
        s_rx.active = true;
        s_rx.meta = *meta;
        s_rx.expected_toggle = !meta->toggle;
        append_frame_payload(&s_rx, frame);
        return;
    }

    if (!s_rx.active || s_rx.meta.source_node_id != meta->source_node_id ||
        s_rx.meta.data_type_id != meta->data_type_id ||
        s_rx.meta.transfer_id != meta->transfer_id ||
        s_rx.expected_toggle != meta->toggle) {
        return;
    }

    if (!append_frame_payload(&s_rx, frame)) {
        return;
    }
    s_rx.expected_toggle = !s_rx.expected_toggle;

    if (meta->end_of_transfer) {
        // Multi-frame DroneCAN transfers carry the 16-bit transfer CRC before the serialized payload.
        if (s_rx.payload_len > 2) {
            handle_transfer(&s_rx.meta, &s_rx.payload[2], s_rx.payload_len - 2);
        }
        s_rx.active = false;
    }
}

static void handle_frame(const twai_message_t *frame)
{
    dronecan_transfer_meta_t meta = {0};
    if (!decode_transfer_meta(frame, &meta)) {
        return;
    }

    if (!CONFIG_REMOTEID_DRONECAN_ACCEPT_ANONYMOUS && meta.source_node_id == 0) {
        return;
    }

    if (meta.service_not_message) {
        ESP_LOGD(TAG, "service transfer src=%u type=%u tid=%u",
                 meta.source_node_id, meta.data_type_id, meta.transfer_id);
        return;
    }

    const char *name = known_message_name(meta.data_type_id);
    if (name) {
        ESP_LOGD(TAG, "message %s src=%u priority=%u tid=%u single=%d dlc=%u",
                 name, meta.source_node_id, meta.priority, meta.transfer_id,
                 meta.start_of_transfer && meta.end_of_transfer,
                 frame->data_length_code);
    } else {
        ESP_LOGV(TAG, "message type=%u src=%u priority=%u tid=%u single=%d dlc=%u",
                 meta.data_type_id, meta.source_node_id, meta.priority,
                 meta.transfer_id, meta.start_of_transfer && meta.end_of_transfer,
                 frame->data_length_code);
    }

    handle_message_frame(frame, &meta);
}

static void remoteid_dronecan_task(void *arg)
{
    (void)arg;
    twai_message_t frame;

    while (true) {
        esp_err_t err = twai_receive(&frame, pdMS_TO_TICKS(1000));
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TWAI receive failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        handle_frame(&frame);
    }
}

esp_err_t remoteid_dronecan_start(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_DRONECAN_RX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "DroneCAN RX GPIO must be configured when DroneCAN input is enabled");
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_DRONECAN_TX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "DroneCAN TX GPIO must be configured when DroneCAN input is enabled");

    twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(
        CONFIG_REMOTEID_DRONECAN_TX_GPIO,
        CONFIG_REMOTEID_DRONECAN_RX_GPIO,
#if CONFIG_REMOTEID_DRONECAN_LISTEN_ONLY
        TWAI_MODE_LISTEN_ONLY);
#else
        TWAI_MODE_NORMAL);
#endif
    twai_timing_config_t timing_config = dronecan_timing_config();
    twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_RETURN_ON_ERROR(twai_driver_install(&general_config, &timing_config, &filter_config),
                        TAG, "install DroneCAN TWAI driver");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "start DroneCAN TWAI driver");

    if (xTaskCreate(remoteid_dronecan_task, "remoteid_dronecan", CONFIG_REMOTEID_DRONECAN_TASK_STACK,
                    NULL, REMOTEID_DRONECAN_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create DroneCAN input task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DroneCAN input started in %s mode on TWAI RX GPIO%d TX GPIO%d node_id=%d",
#if CONFIG_REMOTEID_DRONECAN_LISTEN_ONLY
             "listen-only",
#else
             "normal",
#endif
             CONFIG_REMOTEID_DRONECAN_RX_GPIO,
             CONFIG_REMOTEID_DRONECAN_TX_GPIO,
             CONFIG_REMOTEID_DRONECAN_NODE_ID);
    return ESP_OK;
}

#else

esp_err_t remoteid_dronecan_start(void)
{
    return ESP_OK;
}

#endif
