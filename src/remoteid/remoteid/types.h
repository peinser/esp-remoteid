#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "opendroneid.h"

typedef enum remoteid_message_index {
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_OPERATOR_ID,
    REMOTEID_MESSAGE_SELF_ID,
    REMOTEID_MESSAGE_COUNT,
    // Auth page slots follow the base messages; not passed to encode functions.
    // One entry per ODID auth page (0 .. ODID_AUTH_MAX_PAGES - 1).
    REMOTEID_MESSAGE_AUTH_0 = REMOTEID_MESSAGE_COUNT,
    REMOTEID_MESSAGE_AUTH_1,
    REMOTEID_MESSAGE_AUTH_2,
    REMOTEID_MESSAGE_AUTH_3,
} remoteid_message_index_t;

typedef struct remoteid_state {
    // Basic ID. uas_id holds the 20 raw ID bytes plus an always-zero terminator:
    // NUL-padded ASCII for serial / CAA IDs, binary for UUID / session IDs.
    char uas_id[ODID_ID_SIZE + 1];
    char operator_id[ODID_ID_SIZE + 1];
    ODID_idtype_t id_type;
    ODID_uatype_t ua_type;

    // Location
    bool has_position;
    double latitude;
    double longitude;
    float altitude_geo_m;
    float altitude_baro_m;
    ODID_status_t status;
    float speed_horizontal;
    float speed_vertical;
    float direction;
    float height;
    ODID_Height_reference_t height_type;
    ODID_Horizontal_accuracy_t horiz_acc;
    ODID_Vertical_accuracy_t vert_acc;
    ODID_Vertical_accuracy_t baro_acc;
    ODID_Speed_accuracy_t speed_acc;
    ODID_Timestamp_accuracy_t ts_acc;
    float timestamp;                        // seconds after the UTC hour, or < 0 if unknown
    // esp_timer milliseconds when the last Location update arrived; 0 = never.
    uint32_t location_updated_ms;

    // System / operator
    bool has_operator_position;
    double operator_latitude;
    double operator_longitude;
    float operator_altitude_geo_m;
    ODID_operator_location_type_t operator_location_type;
    uint16_t area_count;
    uint16_t area_radius;
    float area_ceiling_m;
    float area_floor_m;
    ODID_classification_type_t classification_type;
    ODID_category_EU_t eu_category;
    ODID_class_EU_t eu_class;
    // Source timestamp (seconds since 2019-01-01 UTC) of the operator position,
    // and the esp_timer milliseconds when that timestamp last advanced. A relay
    // repeating a cached packet does not advance it, so a live (LIVE_GNSS)
    // operator location still expires when the GCS stops sending.
    uint32_t operator_position_timestamp;
    uint32_t operator_position_updated_ms;
    // Set when a live operator position expired and was replaced by the
    // take-off point (or cleared). Only a newer source timestamp ends it, so a
    // relay repeating its cached SYSTEM cannot bring the stale position back.
    bool operator_position_expired;
    // Take-off point captured at arming: the operator position when no other
    // is available (EU 2019/945: "or, if not available, the take-off point").
    bool has_takeoff_position;
    double takeoff_latitude;
    double takeoff_longitude;
    float takeoff_altitude_geo_m;

    // Self ID
    bool has_self_id;
    ODID_desctype_t self_id_type;
    char self_id_desc[ODID_STR_SIZE + 1];

    // Authentication pages relayed from MAVLink (CONFIG_REMOTEID_AUTH_RELAY).
    // Broadcast verbatim once page 0 and every page up to ext_auth_last_page
    // belong to the same set, and only while page 0 keeps arriving.
    bool has_ext_auth;
    uint8_t ext_auth_last_page;                       // LastPageIndex from page 0
    uint16_t ext_auth_pages_received;                 // bitmask of received pages
    uint32_t ext_auth_updated_ms;                     // esp_timer ms of the last page 0
    ODID_Auth_data ext_auth_pages[ODID_AUTH_MAX_PAGES];
} remoteid_state_t;

typedef struct remoteid_message_bundle {
    uint8_t messages[REMOTEID_MESSAGE_COUNT][ODID_MESSAGE_SIZE];
    uint8_t auth_pages[ODID_AUTH_MAX_PAGES][ODID_MESSAGE_SIZE];
} remoteid_message_bundle_t;
