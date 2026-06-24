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
    // Auth page indices start after base messages; not passed to encode functions.
    // One entry per ODID auth page (0 .. ODID_AUTH_MAX_PAGES - 1).
    REMOTEID_MESSAGE_AUTH_0 = REMOTEID_MESSAGE_COUNT,
    REMOTEID_MESSAGE_AUTH_1,
    REMOTEID_MESSAGE_AUTH_2,
    REMOTEID_MESSAGE_AUTH_3,
    REMOTEID_MESSAGE_AUTH_4,
    REMOTEID_MESSAGE_AUTH_5,
    REMOTEID_MESSAGE_AUTH_6,
    REMOTEID_MESSAGE_AUTH_7,
    REMOTEID_MESSAGE_AUTH_8,
    REMOTEID_MESSAGE_AUTH_9,
    REMOTEID_MESSAGE_AUTH_10,
    REMOTEID_MESSAGE_AUTH_11,
    REMOTEID_MESSAGE_AUTH_12,
    REMOTEID_MESSAGE_AUTH_13,
    REMOTEID_MESSAGE_AUTH_14,
    REMOTEID_MESSAGE_AUTH_15,
} remoteid_message_index_t;

typedef struct remoteid_state {
    // Basic ID
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

    // Self ID
    bool has_self_id;
    ODID_desctype_t self_id_type;
    char self_id_desc[ODID_STR_SIZE + 1];

    // Authentication received over the input link (MAVLink/DroneCAN). When
    // has_ext_auth is set, these pages are broadcast verbatim and supersede any
    // locally generated signature. Pages are accumulated until 0..ext_auth_last_page
    // have all been received.
    bool has_ext_auth;
    uint8_t ext_auth_last_page;                       // LastPageIndex from page 0
    uint16_t ext_auth_pages_received;                 // bitmask of received pages
    ODID_Auth_data ext_auth_pages[ODID_AUTH_MAX_PAGES];
} remoteid_state_t;

typedef struct remoteid_message_bundle {
    uint8_t messages[REMOTEID_MESSAGE_COUNT][ODID_MESSAGE_SIZE];
    uint8_t auth_pages[ODID_AUTH_MAX_PAGES][ODID_MESSAGE_SIZE];
} remoteid_message_bundle_t;
