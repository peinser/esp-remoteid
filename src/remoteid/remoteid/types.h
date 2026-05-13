#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "opendroneid.h"

typedef enum remoteid_message_index {
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_OPERATOR_ID,
    REMOTEID_MESSAGE_COUNT,
} remoteid_message_index_t;

typedef struct remoteid_state {
    char uas_id[ODID_ID_SIZE + 1];
    char operator_id[ODID_ID_SIZE + 1];
    ODID_idtype_t id_type;
    ODID_uatype_t ua_type;
    ODID_category_EU_t eu_category;
    ODID_class_EU_t eu_class;
    bool has_position;
    double latitude;
    double longitude;
    float altitude_m;
} remoteid_state_t;

typedef struct remoteid_message_bundle {
    uint8_t messages[REMOTEID_MESSAGE_COUNT][ODID_MESSAGE_SIZE];
} remoteid_message_bundle_t;
