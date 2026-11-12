#include "dronecan.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_DRONECAN_INPUT

// DroneCAN node for ArduPilot's OpenDroneID CAN backend (DID_CANDRIVER):
//   in:  dronecan.remoteid.BasicID / Location / SelfID / System / OperatorID
//   out: dronecan.remoteid.ArmStatus and uavcan.protocol.NodeStatus at 1 Hz,
//        uavcan.protocol.GetNodeInfo responses.
// Field layouts follow the DSDL in the DroneCAN repository; the messages carry
// the MAVLink OPEN_DRONE_ID_* fields, so they are converted by input.c.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "clock.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "store.h"

#define REMOTEID_DRONECAN_TASK_PRIORITY 5
#define REMOTEID_DRONECAN_RX_QUEUE_LEN  32
// Room for a whole GetNodeInfo response (9 frames) plus the periodic messages.
#define REMOTEID_DRONECAN_TX_QUEUE_LEN  16
#define REMOTEID_DRONECAN_EXT_ID_MASK   0x1FFFFFFFUL
#define REMOTEID_DRONECAN_TAIL_START    BIT(7)
#define REMOTEID_DRONECAN_TAIL_END      BIT(6)
#define REMOTEID_DRONECAN_TAIL_TOGGLE   BIT(5)
#define REMOTEID_DRONECAN_TRANSFER_ID_MASK 0x1FU
#define REMOTEID_DRONECAN_MAX_TRANSFER_PAYLOAD 256
// Concurrent multi-frame transfers (one per source node and data type).
#define REMOTEID_DRONECAN_RX_SLOTS      4
// DroneCAN transfer timeout: a partial transfer older than this is abandoned.
#define REMOTEID_DRONECAN_TRANSFER_TIMEOUT_US 2000000
#define REMOTEID_DRONECAN_STATUS_PERIOD_US    1000000
// libcanard CANARD_TRANSFER_PRIORITY_LOW, as ArduPilot uses for these messages.
#define REMOTEID_DRONECAN_PRIORITY_LOW 24

#define DTID_GET_NODE_INFO 1      // service
#define DTID_NODE_STATUS   341
#define DTID_RID_BASIC_ID    20030
#define DTID_RID_LOCATION    20031
#define DTID_RID_SELF_ID     20032
#define DTID_RID_SYSTEM      20033
#define DTID_RID_OPERATOR_ID 20034
#define DTID_RID_ARM_STATUS  20035

// Data type signatures seed the multi-frame transfer CRC (DSDL compiler output).
#define SIGNATURE_GET_NODE_INFO   0xEE468A8121C46A9EULL
#define SIGNATURE_RID_BASIC_ID    0x5B1C624A8E4FC533ULL
#define SIGNATURE_RID_LOCATION    0xEAA3A2C5BCB14CAAULL
#define SIGNATURE_RID_SELF_ID     0x59BE81DC4C06A185ULL
#define SIGNATURE_RID_SYSTEM      0x9AC872F49BF32437ULL
#define SIGNATURE_RID_OPERATOR_ID 0x581E7FC7F03AF935ULL
#define SIGNATURE_RID_ARM_STATUS  0xFEDF72CCF06F3BDDULL

#define NODE_HEALTH_OK         0
#define NODE_HEALTH_CRITICAL   3
#define NODE_MODE_OPERATIONAL  0
#define NODE_NAME "com.peinser.remoteid"

#define SOURCE "DroneCAN"

static const char *TAG = "remoteid_dronecan";

typedef struct dronecan_transfer_meta {
    uint8_t priority;
    uint16_t data_type_id;          // message type, or service type for services
    uint8_t source_node_id;
    bool service;
    bool request;                   // services only
    uint8_t destination_node_id;    // services only
    bool start_of_transfer;
    bool end_of_transfer;
    bool toggle;
    uint8_t transfer_id;
} dronecan_transfer_meta_t;

typedef struct dronecan_rx_transfer {
    bool active;
    int64_t started_us;
    dronecan_transfer_meta_t meta;
    uint8_t payload[REMOTEID_DRONECAN_MAX_TRANSFER_PAYLOAD];
    size_t payload_len;
    bool expected_toggle;
} dronecan_rx_transfer_t;

// Sequential reader over a DroneCAN payload. Reading past the end sets overrun.
typedef struct dronecan_reader {
    const uint8_t *data;
    size_t bit_len;
    size_t offset;
    bool overrun;
} dronecan_reader_t;

static dronecan_rx_transfer_t s_rx[REMOTEID_DRONECAN_RX_SLOTS];
// The node whose Remote ID data this module broadcasts: the first node that
// sends dronecan.remoteid.* (the autopilot). Never re-bound.
static uint8_t s_autopilot_node;
static uint8_t s_node_status_transfer_id;
static uint8_t s_arm_status_transfer_id;

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

// ---- Transfer CRC ------------------------------------------------------------

// CRC-16-CCITT (poly 0x1021, init 0xFFFF), as used for DroneCAN transfer CRCs.
static uint16_t crc16_add(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// Multi-frame transfer CRC: over the data type signature (little-endian), then the payload.
static uint16_t transfer_crc(uint64_t signature, const uint8_t *payload, size_t len)
{
    uint8_t signature_bytes[8];
    for (int i = 0; i < 8; i++) {
        signature_bytes[i] = (uint8_t)(signature >> (8 * i));
    }
    return crc16_add(crc16_add(0xFFFFU, signature_bytes, sizeof(signature_bytes)), payload, len);
}

// ---- Payload reader ----------------------------------------------------------

// DroneCAN (UAVCAN v0) bit order, as in libcanard's canardDecodeScalar: bits
// are taken MSB-first from each payload byte, the collected bytes form a
// little-endian value, and a trailing partial byte is right-aligned. This only
// matches plain LSB-first reading for byte-aligned fields.
static uint64_t read_bits(const dronecan_reader_t *reader, size_t bit_offset, size_t bit_count)
{
    uint8_t bytes[8] = {0};
    for (size_t i = 0; i < bit_count; i++) {
        size_t source_bit = bit_offset + i;
        if (reader->data[source_bit / 8] & (0x80U >> (source_bit % 8))) {
            bytes[i / 8] |= (uint8_t)(0x80U >> (i % 8));
        }
    }
    if (bit_count % 8 != 0) {
        bytes[bit_count / 8] >>= 8 - bit_count % 8;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        value |= (uint64_t)bytes[i] << (8 * i);
    }
    return value;
}

static uint64_t take_unsigned(dronecan_reader_t *reader, size_t bit_count)
{
    if (reader->overrun || reader->offset + bit_count > reader->bit_len) {
        reader->overrun = true;
        return 0;
    }
    uint64_t value = read_bits(reader, reader->offset, bit_count);
    reader->offset += bit_count;
    return value;
}

static int64_t take_signed(dronecan_reader_t *reader, size_t bit_count)
{
    uint64_t value = take_unsigned(reader, bit_count);
    if ((value & (UINT64_C(1) << (bit_count - 1))) == 0) {
        return (int64_t)value;
    }
    return (int64_t)(value | (~UINT64_C(0) << bit_count));
}

static float take_float(dronecan_reader_t *reader)
{
    uint32_t raw = (uint32_t)take_unsigned(reader, 32);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

// uint8[<=max] that is not the last field: a length prefix of length_bits, then
// the bytes. Unused destination bytes stay zero.
static void take_array(dronecan_reader_t *reader, uint8_t *dst, size_t max, size_t length_bits)
{
    size_t count = (size_t)take_unsigned(reader, length_bits);
    if (count > max) {
        reader->overrun = true;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = (uint8_t)take_unsigned(reader, 8);
    }
}

// uint8[<=max] as the last field (tail array optimisation): no length prefix,
// the array runs to the end of the payload; fewer than 8 leftover bits are padding.
static void take_tail_array(dronecan_reader_t *reader, uint8_t *dst, size_t max)
{
    size_t count = (reader->bit_len - reader->offset) / 8;
    if (reader->overrun || count > max) {
        reader->overrun = true;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        dst[i] = (uint8_t)take_unsigned(reader, 8);
    }
}

// Every dronecan.remoteid message starts with uint8[<=20] id_or_mac (5-bit length).
static void take_id_or_mac(dronecan_reader_t *reader, uint8_t id_or_mac[20])
{
    take_array(reader, id_or_mac, 20, 5);
}

// ---- Remote ID messages ------------------------------------------------------

// A decoded message is used when it was complete and describes this aircraft.
static bool usable(const dronecan_reader_t *reader, const uint8_t id_or_mac[20], const char *name)
{
    if (reader->overrun) {
        ESP_LOGW(TAG, "ignored malformed %s", name);
        return false;
    }
    if (remoteid_input_describes_other_aircraft(id_or_mac)) {
        ESP_LOGD(TAG, "ignored %s describing another aircraft", name);
        return false;
    }
    return true;
}

static void handle_basic_id(dronecan_reader_t *reader)
{
    mavlink_open_drone_id_basic_id_t msg = { 0 };
    take_id_or_mac(reader, msg.id_or_mac);
    msg.id_type = (uint8_t)take_unsigned(reader, 8);
    msg.ua_type = (uint8_t)take_unsigned(reader, 8);
    take_tail_array(reader, msg.uas_id, sizeof(msg.uas_id));
    if (usable(reader, msg.id_or_mac, "BasicID")) {
        remoteid_input_basic_id(&msg, SOURCE);
    }
}

static void handle_location(dronecan_reader_t *reader)
{
    mavlink_open_drone_id_location_t msg = { 0 };
    take_id_or_mac(reader, msg.id_or_mac);
    msg.status              = (uint8_t)take_unsigned(reader, 8);
    msg.direction           = (uint16_t)take_unsigned(reader, 16);
    msg.speed_horizontal    = (uint16_t)take_unsigned(reader, 16);
    msg.speed_vertical      = (int16_t)take_signed(reader, 16);
    msg.latitude            = (int32_t)take_signed(reader, 32);
    msg.longitude           = (int32_t)take_signed(reader, 32);
    msg.altitude_barometric = take_float(reader);
    msg.altitude_geodetic   = take_float(reader);
    msg.height_reference    = (uint8_t)take_unsigned(reader, 8);
    msg.height              = take_float(reader);
    msg.horizontal_accuracy = (uint8_t)take_unsigned(reader, 8);
    msg.vertical_accuracy   = (uint8_t)take_unsigned(reader, 8);
    msg.barometer_accuracy  = (uint8_t)take_unsigned(reader, 8);
    msg.speed_accuracy      = (uint8_t)take_unsigned(reader, 8);
    msg.timestamp           = take_float(reader);
    msg.timestamp_accuracy  = (uint8_t)take_unsigned(reader, 8);
    if (usable(reader, msg.id_or_mac, "Location")) {
        remoteid_input_location(&msg, SOURCE);
    }
}

static void handle_self_id(dronecan_reader_t *reader)
{
    mavlink_open_drone_id_self_id_t msg = { 0 };
    take_id_or_mac(reader, msg.id_or_mac);
    msg.description_type = (uint8_t)take_unsigned(reader, 8);
    take_tail_array(reader, (uint8_t *)msg.description, sizeof(msg.description));
    if (usable(reader, msg.id_or_mac, "SelfID")) {
        remoteid_input_self_id(&msg, SOURCE);
    }
}

static void handle_system(dronecan_reader_t *reader)
{
    mavlink_open_drone_id_system_t msg = { 0 };
    take_id_or_mac(reader, msg.id_or_mac);
    msg.operator_location_type = (uint8_t)take_unsigned(reader, 8);
    msg.classification_type    = (uint8_t)take_unsigned(reader, 8);
    msg.operator_latitude      = (int32_t)take_signed(reader, 32);
    msg.operator_longitude     = (int32_t)take_signed(reader, 32);
    msg.area_count             = (uint16_t)take_unsigned(reader, 16);
    msg.area_radius            = (uint16_t)take_unsigned(reader, 16);
    msg.area_ceiling           = take_float(reader);
    msg.area_floor             = take_float(reader);
    msg.category_eu            = (uint8_t)take_unsigned(reader, 8);
    msg.class_eu               = (uint8_t)take_unsigned(reader, 8);
    msg.operator_altitude_geo  = take_float(reader);
    msg.timestamp              = (uint32_t)take_unsigned(reader, 32);
    if (!usable(reader, msg.id_or_mac, "System")) {
        return;
    }
    // ArduPilot sends no UTC time over DroneCAN other than this timestamp (the
    // ground station's, in seconds). It seeds the clock once; later values would
    // step it back and forth by up to a second.
    if (!remoteid_clock_is_synced() && msg.timestamp != 0) {
        remoteid_clock_set_utc_us(((uint64_t)msg.timestamp + REMOTEID_ODID_EPOCH_2019) * 1000000ULL);
        ESP_LOGI(TAG, "UTC clock seeded from the System timestamp");
    }
    remoteid_input_system(&msg, SOURCE);
}

static void handle_operator_id(dronecan_reader_t *reader)
{
    mavlink_open_drone_id_operator_id_t msg = { 0 };
    take_id_or_mac(reader, msg.id_or_mac);
    msg.operator_id_type = (uint8_t)take_unsigned(reader, 8);
    take_tail_array(reader, (uint8_t *)msg.operator_id, sizeof(msg.operator_id));
    if (usable(reader, msg.id_or_mac, "OperatorID")) {
        remoteid_input_operator_id(&msg, SOURCE);
    }
}

static bool rid_message_signature(uint16_t data_type_id, uint64_t *signature)
{
    switch (data_type_id) {
    case DTID_RID_BASIC_ID:    *signature = SIGNATURE_RID_BASIC_ID;    return true;
    case DTID_RID_LOCATION:    *signature = SIGNATURE_RID_LOCATION;    return true;
    case DTID_RID_SELF_ID:     *signature = SIGNATURE_RID_SELF_ID;     return true;
    case DTID_RID_SYSTEM:      *signature = SIGNATURE_RID_SYSTEM;      return true;
    case DTID_RID_OPERATOR_ID: *signature = SIGNATURE_RID_OPERATOR_ID; return true;
    default:                   return false;
    }
}

static void handle_rid_message(const dronecan_transfer_meta_t *meta, const uint8_t *payload, size_t len)
{
    if (s_autopilot_node == 0) {
        s_autopilot_node = meta->source_node_id;
        ESP_LOGI(TAG, "bound to Remote ID source node %u", s_autopilot_node);
    } else if (meta->source_node_id != s_autopilot_node) {
        ESP_LOGD(TAG, "ignored Remote ID data from node %u (bound to %u)", meta->source_node_id, s_autopilot_node);
        return;
    }

    dronecan_reader_t reader = { .data = payload, .bit_len = len * 8 };
    switch (meta->data_type_id) {
    case DTID_RID_BASIC_ID:    handle_basic_id(&reader);    break;
    case DTID_RID_LOCATION:    handle_location(&reader);    break;
    case DTID_RID_SELF_ID:     handle_self_id(&reader);     break;
    case DTID_RID_SYSTEM:      handle_system(&reader);      break;
    case DTID_RID_OPERATOR_ID: handle_operator_id(&reader); break;
    default:                   break;
    }
}

// ---- Transmit ----------------------------------------------------------------

static uint32_t message_can_id(uint8_t priority, uint16_t data_type_id)
{
    return ((uint32_t)priority << 24) | ((uint32_t)data_type_id << 8) | CONFIG_REMOTEID_DRONECAN_NODE_ID;
}

static uint32_t response_can_id(uint8_t priority, uint8_t service_type_id, uint8_t destination_node_id)
{
    return ((uint32_t)priority << 24) | ((uint32_t)service_type_id << 16) |
           ((uint32_t)destination_node_id << 8) | BIT(7) | CONFIG_REMOTEID_DRONECAN_NODE_ID;
}

// Send one transfer: a single frame when it fits, otherwise CRC + payload split
// over frames with alternating toggle bits.
static void send_transfer(uint32_t can_id, uint64_t signature, uint8_t transfer_id,
                          const uint8_t *payload, size_t len)
{
    uint8_t buffer[2 + REMOTEID_DRONECAN_MAX_TRANSFER_PAYLOAD];
    const uint8_t *data = payload;
    size_t data_len = len;
    if (len > 7) {
        uint16_t crc = transfer_crc(signature, payload, len);
        buffer[0] = (uint8_t)crc;
        buffer[1] = (uint8_t)(crc >> 8);
        memcpy(&buffer[2], payload, len);
        data = buffer;
        data_len = len + 2;
    }

    twai_message_t frame = { .extd = 1, .identifier = can_id };
    bool toggle = false;
    for (size_t offset = 0; offset < data_len; offset += 7) {
        size_t chunk = data_len - offset < 7 ? data_len - offset : 7;
        memcpy(frame.data, data + offset, chunk);
        frame.data[chunk] = (uint8_t)((offset == 0 ? REMOTEID_DRONECAN_TAIL_START : 0) |
                                      (offset + chunk == data_len ? REMOTEID_DRONECAN_TAIL_END : 0) |
                                      (toggle ? REMOTEID_DRONECAN_TAIL_TOGGLE : 0) |
                                      (transfer_id & REMOTEID_DRONECAN_TRANSFER_ID_MASK));
        frame.data_length_code = chunk + 1;
        if (twai_transmit(&frame, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGD(TAG, "TWAI transmit failed, dropping transfer 0x%08" PRIx32, can_id);
            return;
        }
        toggle = !toggle;
    }
}

// uavcan.protocol.NodeStatus: uptime_sec uint32, health uint2, mode uint3,
// sub_mode uint3, vendor_specific_status_code uint16.
static void write_node_status(uint8_t out[7], remoteid_health_t health)
{
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint8_t node_health = health == REMOTEID_HEALTH_SELFTEST_FAILED ? NODE_HEALTH_CRITICAL : NODE_HEALTH_OK;
    out[0] = (uint8_t)uptime_s;
    out[1] = (uint8_t)(uptime_s >> 8);
    out[2] = (uint8_t)(uptime_s >> 16);
    out[3] = (uint8_t)(uptime_s >> 24);
    out[4] = (uint8_t)((node_health << 6) | (NODE_MODE_OPERATIONAL << 3));
    out[5] = 0;
    out[6] = 0;
}

// NodeStatus (mandatory for every DroneCAN node) and ArmStatus, which ArduPilot
// requires at least every 3 s before it allows arming.
static void send_status(void)
{
    remoteid_health_t health = remoteid_store_health();

    uint8_t node_status[7];
    write_node_status(node_status, health);
    send_transfer(message_can_id(REMOTEID_DRONECAN_PRIORITY_LOW, DTID_NODE_STATUS), 0,
                  s_node_status_transfer_id++, node_status, sizeof(node_status));

    // dronecan.remoteid.ArmStatus: status uint8, error uint8[<=50] (tail array).
    const char *error = remoteid_store_health_text(health);
    uint8_t arm_status[1 + 50];
    size_t error_len = strnlen(error, 50);
    arm_status[0] = health == REMOTEID_HEALTH_OK ? MAV_ODID_ARM_STATUS_GOOD_TO_ARM
                                                 : MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC;
    memcpy(&arm_status[1], error, error_len);
    send_transfer(message_can_id(REMOTEID_DRONECAN_PRIORITY_LOW, DTID_RID_ARM_STATUS), SIGNATURE_RID_ARM_STATUS,
                  s_arm_status_transfer_id++, arm_status, 1 + error_len);
}

// uavcan.protocol.GetNodeInfo response: NodeStatus, SoftwareVersion (15 bytes,
// all zero: no optional fields), HardwareVersion (major, minor, unique_id[16],
// certificate_of_authenticity length 0), name (tail array).
static void respond_node_info(const dronecan_transfer_meta_t *request)
{
    uint8_t response[7 + 15 + 2 + 16 + 1 + sizeof(NODE_NAME) - 1] = {0};
    write_node_status(response, remoteid_store_health());

    uint8_t *unique_id = &response[7 + 15 + 2];
    esp_efuse_mac_get_default(unique_id);   // 6 of the 16 bytes; the rest stay zero
    memcpy(&response[7 + 15 + 2 + 16 + 1], NODE_NAME, sizeof(NODE_NAME) - 1);

    send_transfer(response_can_id(request->priority, DTID_GET_NODE_INFO, request->source_node_id),
                  SIGNATURE_GET_NODE_INFO, request->transfer_id, response, sizeof(response));
}

// ---- Receive -----------------------------------------------------------------

static bool decode_transfer_meta(const twai_message_t *frame, dronecan_transfer_meta_t *meta)
{
    if (!frame->extd || frame->rtr || frame->data_length_code == 0) {
        return false;
    }

    uint32_t can_id = frame->identifier & REMOTEID_DRONECAN_EXT_ID_MASK;
    meta->priority = (uint8_t)((can_id >> 24) & 0x1FU);
    meta->service = (can_id & BIT(7)) != 0;
    meta->source_node_id = (uint8_t)(can_id & 0x7FU);
    if (meta->service) {
        meta->data_type_id = (uint16_t)((can_id >> 16) & 0xFFU);
        meta->request = (can_id & BIT(15)) != 0;
        meta->destination_node_id = (uint8_t)((can_id >> 8) & 0x7FU);
    } else {
        meta->data_type_id = (uint16_t)((can_id >> 8) & 0xFFFFU);
    }

    uint8_t tail = frame->data[frame->data_length_code - 1];
    meta->start_of_transfer = (tail & REMOTEID_DRONECAN_TAIL_START) != 0;
    meta->end_of_transfer = (tail & REMOTEID_DRONECAN_TAIL_END) != 0;
    meta->toggle = (tail & REMOTEID_DRONECAN_TAIL_TOGGLE) != 0;
    meta->transfer_id = tail & REMOTEID_DRONECAN_TRANSFER_ID_MASK;
    return true;
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

// The reassembly slot for (source node, data type). With start set, a new
// transfer takes a free slot or, if none is free, the oldest one.
static dronecan_rx_transfer_t *rx_slot(const dronecan_transfer_meta_t *meta, bool start)
{
    dronecan_rx_transfer_t *free_slot = NULL;
    dronecan_rx_transfer_t *oldest = &s_rx[0];
    int64_t now_us = esp_timer_get_time();

    for (size_t i = 0; i < REMOTEID_DRONECAN_RX_SLOTS; i++) {
        dronecan_rx_transfer_t *slot = &s_rx[i];
        if (slot->active && now_us - slot->started_us > REMOTEID_DRONECAN_TRANSFER_TIMEOUT_US) {
            slot->active = false;
        }
        if (slot->active && slot->meta.source_node_id == meta->source_node_id &&
            slot->meta.data_type_id == meta->data_type_id) {
            return slot;
        }
        if (!slot->active && !free_slot) {
            free_slot = slot;
        }
        if (slot->started_us < oldest->started_us) {
            oldest = slot;
        }
    }
    if (!start) {
        return NULL;
    }
    return free_slot ? free_slot : oldest;
}

static void handle_message_frame(const twai_message_t *frame, const dronecan_transfer_meta_t *meta)
{
    uint64_t signature;
    if (!rid_message_signature(meta->data_type_id, &signature)) {
        return;
    }

    if (meta->start_of_transfer && meta->end_of_transfer) {
        handle_rid_message(meta, frame->data, frame->data_length_code - 1);
        return;
    }

    dronecan_rx_transfer_t *transfer = rx_slot(meta, meta->start_of_transfer);
    if (!transfer) {
        return;
    }

    if (meta->start_of_transfer) {
        memset(transfer, 0, sizeof(*transfer));
        transfer->active = true;
        transfer->started_us = esp_timer_get_time();
        transfer->meta = *meta;
        transfer->expected_toggle = !meta->toggle;
        append_frame_payload(transfer, frame);
        return;
    }

    if (transfer->meta.transfer_id != meta->transfer_id || transfer->expected_toggle != meta->toggle) {
        return;
    }

    if (!append_frame_payload(transfer, frame)) {
        return;
    }
    transfer->expected_toggle = !transfer->expected_toggle;

    if (meta->end_of_transfer) {
        transfer->active = false;
        uint16_t received_crc = (uint16_t)(transfer->payload[0] | (transfer->payload[1] << 8));
        if (transfer->payload_len <= 2 ||
            transfer_crc(signature, &transfer->payload[2], transfer->payload_len - 2) != received_crc) {
            ESP_LOGW(TAG, "dropped DroneCAN transfer with bad CRC type=%u src=%u",
                     transfer->meta.data_type_id, transfer->meta.source_node_id);
            return;
        }
        handle_rid_message(&transfer->meta, &transfer->payload[2], transfer->payload_len - 2);
    }
}

static void handle_frame(const twai_message_t *frame)
{
    dronecan_transfer_meta_t meta = {0};
    // Anonymous nodes (ID 0) only take part in node ID allocation.
    if (!decode_transfer_meta(frame, &meta) || meta.source_node_id == 0) {
        return;
    }

    if (meta.service) {
        // GetNodeInfo requests carry no payload, so they are always single-frame.
        if (meta.request && meta.data_type_id == DTID_GET_NODE_INFO &&
            meta.destination_node_id == CONFIG_REMOTEID_DRONECAN_NODE_ID &&
            meta.start_of_transfer && meta.end_of_transfer) {
            respond_node_info(&meta);
        }
        return;
    }

    handle_message_frame(frame, &meta);
}

// A bus-off controller receives and sends nothing until recovered and restarted.
static void recover_bus_if_needed(void)
{
    twai_status_info_t info;
    if (twai_get_status_info(&info) != ESP_OK) {
        return;
    }
    if (info.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGW(TAG, "TWAI bus-off, starting recovery");
        twai_initiate_recovery();
    } else if (info.state == TWAI_STATE_STOPPED) {
        ESP_LOGI(TAG, "TWAI recovered, restarting");
        twai_start();
    }
}

static void remoteid_dronecan_task(void *arg)
{
    (void)arg;
    int64_t next_status_us = 0;

    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_status_us) {
            next_status_us = now_us + REMOTEID_DRONECAN_STATUS_PERIOD_US;
            recover_bus_if_needed();
            send_status();
        }

        twai_message_t frame;
        esp_err_t err = twai_receive(&frame, pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            handle_frame(&frame);
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "TWAI receive failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t remoteid_dronecan_start(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_DRONECAN_RX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "DroneCAN RX GPIO must be configured when DroneCAN is enabled");
    ESP_RETURN_ON_FALSE(CONFIG_REMOTEID_DRONECAN_TX_GPIO >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "DroneCAN TX GPIO must be configured when DroneCAN is enabled");

    twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(
        CONFIG_REMOTEID_DRONECAN_TX_GPIO, CONFIG_REMOTEID_DRONECAN_RX_GPIO, TWAI_MODE_NORMAL);
    // The default 5-frame RX queue overflows under normal bus load, which loses
    // frames of multi-frame transfers.
    general_config.rx_queue_len = REMOTEID_DRONECAN_RX_QUEUE_LEN;
    general_config.tx_queue_len = REMOTEID_DRONECAN_TX_QUEUE_LEN;
    twai_timing_config_t timing_config = dronecan_timing_config();
    twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_RETURN_ON_ERROR(twai_driver_install(&general_config, &timing_config, &filter_config),
                        TAG, "install DroneCAN TWAI driver");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "start DroneCAN TWAI driver");

    if (xTaskCreate(remoteid_dronecan_task, "remoteid_dronecan", CONFIG_REMOTEID_DRONECAN_TASK_STACK,
                    NULL, REMOTEID_DRONECAN_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create DroneCAN task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DroneCAN node %d started on TWAI RX GPIO%d TX GPIO%d",
             CONFIG_REMOTEID_DRONECAN_NODE_ID, CONFIG_REMOTEID_DRONECAN_RX_GPIO, CONFIG_REMOTEID_DRONECAN_TX_GPIO);
    return ESP_OK;
}

#else

esp_err_t remoteid_dronecan_start(void)
{
    return ESP_OK;
}

#endif
