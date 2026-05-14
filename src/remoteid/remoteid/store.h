#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "types.h"

typedef enum remoteid_store_update_type {
    REMOTEID_STORE_UPDATE_BASIC_ID,
    REMOTEID_STORE_UPDATE_OPERATOR_ID,
    REMOTEID_STORE_UPDATE_LOCATION,
} remoteid_store_update_type_t;

typedef struct remoteid_store_update {
    remoteid_store_update_type_t type;
    union {
        struct {
            char uas_id[ODID_ID_SIZE + 1];
            ODID_idtype_t id_type;
            ODID_uatype_t ua_type;
        } basic_id;
        struct {
            char operator_id[ODID_ID_SIZE + 1];
        } operator_id;
        struct {
            bool has_position;
            double latitude;
            double longitude;
            float altitude_m;
        } location;
    } data;
} remoteid_store_update_t;

esp_err_t remoteid_store_start(const remoteid_state_t *initial_state);
esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout);
esp_err_t remoteid_store_get_snapshot(remoteid_state_t *out);
esp_err_t remoteid_store_wait_ready(TickType_t timeout);
bool remoteid_store_is_ready(void);
