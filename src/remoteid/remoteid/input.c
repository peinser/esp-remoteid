#include "input.h"

#include <string.h>

#include "esp_log.h"
#include "store.h"

static const char *TAG = "remoteid_input";

static void submit(const remoteid_store_update_t *update, const char *source, const char *name)
{
    if (remoteid_store_submit(update, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGD(TAG, "accepted %s %s", source, name);
    } else {
        ESP_LOGW(TAG, "dropped %s %s: state queue full", source, name);
    }
}

// Fixed-size text field, NUL-padded or filling the whole field.
static void copy_text(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t len = strnlen(src, src_len);
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

bool remoteid_input_describes_other_aircraft(const uint8_t id_or_mac[20])
{
    for (size_t i = 0; i < 20; i++) {
        if (id_or_mac[i] != 0) {
            return true;
        }
    }
    return false;
}

void remoteid_input_basic_id(const mavlink_open_drone_id_basic_id_t *msg, const char *source)
{
    remoteid_store_update_t update = {
        .type = REMOTEID_STORE_UPDATE_BASIC_ID,
        .data.basic_id.id_type = (ODID_idtype_t)msg->id_type,
        .data.basic_id.ua_type = (ODID_uatype_t)msg->ua_type,
    };
    // Raw copy: UUID and session IDs are binary; the store validates per ID type.
    memcpy(update.data.basic_id.uas_id, msg->uas_id, ODID_ID_SIZE);
    submit(&update, source, "Basic ID");
}

void remoteid_input_operator_id(const mavlink_open_drone_id_operator_id_t *msg, const char *source)
{
    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_OPERATOR_ID };
    copy_text(update.data.operator_id.operator_id, sizeof(update.data.operator_id.operator_id),
              msg->operator_id, sizeof(msg->operator_id));
    submit(&update, source, "Operator ID");
}

void remoteid_input_location(const mavlink_open_drone_id_location_t *msg, const char *source)
{
    remoteid_store_update_t update = {
        .type                           = REMOTEID_STORE_UPDATE_LOCATION,
        .data.location.has_position     = msg->latitude != 0 || msg->longitude != 0,
        .data.location.latitude         = (double)msg->latitude  / 10000000.0,
        .data.location.longitude        = (double)msg->longitude / 10000000.0,
        .data.location.altitude_geo_m   = msg->altitude_geodetic   <= (float)INV_ALT ? (float)INV_ALT : msg->altitude_geodetic,
        .data.location.altitude_baro_m  = msg->altitude_barometric <= (float)INV_ALT ? (float)INV_ALT : msg->altitude_barometric,
        .data.location.status           = (ODID_status_t)msg->status,
        // Speeds and direction are in hundredths (cm/s, centi-degrees); the
        // unknown values are 25500, 6300 and 36100.
        .data.location.speed_horizontal = msg->speed_horizontal >= 25500 ? (float)INV_SPEED_H
                                                                         : msg->speed_horizontal / 100.0f,
        .data.location.speed_vertical   = msg->speed_vertical >= 6300 ? (float)INV_SPEED_V
                                                                      : msg->speed_vertical / 100.0f,
        .data.location.direction        = msg->direction >= 36100 ? (float)INV_DIR : msg->direction / 100.0f,
        .data.location.height           = msg->height <= (float)INV_ALT ? (float)INV_ALT : msg->height,
        .data.location.height_type      = (ODID_Height_reference_t)msg->height_reference,
        .data.location.horiz_acc        = (ODID_Horizontal_accuracy_t)msg->horizontal_accuracy,
        .data.location.vert_acc         = (ODID_Vertical_accuracy_t)msg->vertical_accuracy,
        .data.location.baro_acc         = (ODID_Vertical_accuracy_t)msg->barometer_accuracy,
        .data.location.speed_acc        = (ODID_Speed_accuracy_t)msg->speed_accuracy,
        .data.location.ts_acc           = (ODID_Timestamp_accuracy_t)msg->timestamp_accuracy,
        .data.location.timestamp        = msg->timestamp >= (float)MAX_TIMESTAMP ? -1.0f : msg->timestamp,
    };
    submit(&update, source, "Location");
}

void remoteid_input_system(const mavlink_open_drone_id_system_t *msg, const char *source)
{
    remoteid_store_update_t update = {
        .type                                = REMOTEID_STORE_UPDATE_SYSTEM,
        .data.system.has_operator_position   = msg->operator_latitude != 0 || msg->operator_longitude != 0,
        .data.system.operator_latitude       = (double)msg->operator_latitude  / 10000000.0,
        .data.system.operator_longitude      = (double)msg->operator_longitude / 10000000.0,
        .data.system.operator_altitude_geo_m = msg->operator_altitude_geo <= (float)INV_ALT ? (float)INV_ALT
                                                                                           : msg->operator_altitude_geo,
        .data.system.operator_location_type  = (ODID_operator_location_type_t)msg->operator_location_type,
        .data.system.area_count              = msg->area_count,
        .data.system.area_radius             = msg->area_radius,
        .data.system.area_ceiling_m          = msg->area_ceiling,
        .data.system.area_floor_m            = msg->area_floor,
        .data.system.classification_type     = (ODID_classification_type_t)msg->classification_type,
        .data.system.eu_category             = (ODID_category_EU_t)msg->category_eu,
        .data.system.eu_class                = (ODID_class_EU_t)msg->class_eu,
        .data.system.timestamp               = msg->timestamp,
    };
    submit(&update, source, "System");
}

void remoteid_input_self_id(const mavlink_open_drone_id_self_id_t *msg, const char *source)
{
    remoteid_store_update_t update = {
        .type                   = REMOTEID_STORE_UPDATE_SELF_ID,
        .data.self_id.desc_type = (ODID_desctype_t)msg->description_type,
    };
    copy_text(update.data.self_id.desc, sizeof(update.data.self_id.desc),
              msg->description, sizeof(msg->description));
    submit(&update, source, "Self ID");
}
