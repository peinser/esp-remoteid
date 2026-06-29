#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "types.h"

typedef enum remoteid_store_update_type {
    REMOTEID_STORE_UPDATE_BASIC_ID,
    REMOTEID_STORE_UPDATE_OPERATOR_ID,
    REMOTEID_STORE_UPDATE_LOCATION,
    REMOTEID_STORE_UPDATE_SYSTEM,
    // Operator position only, from OPEN_DRONE_ID_SYSTEM_UPDATE. Leaves the
    // category/classification/area fields from the last full System untouched.
    REMOTEID_STORE_UPDATE_SYSTEM_UPDATE,
    // Drop a stale live operator position. Carries no payload.
    REMOTEID_STORE_UPDATE_OPERATOR_LOCATION_EXPIRE,
    // Self-test failed: latch a Remote ID system-failure. Carries no payload.
    REMOTEID_STORE_UPDATE_SELFTEST_FAILED,
    REMOTEID_STORE_UPDATE_SELF_ID,
    // A single received Authentication page. The store accumulates pages and
    // marks external auth ready once 0..LastPageIndex have all arrived.
    REMOTEID_STORE_UPDATE_AUTH,
    // Snapshot the current location into the System operator position fields.
    // Carries no payload; the store reads from its own location state atomically.
    REMOTEID_STORE_UPDATE_TAKEOFF,
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
            ODID_desctype_t desc_type;
            char desc[ODID_STR_SIZE + 1];
        } self_id;
        struct {
            // One decoded Authentication page as received from the input link.
            ODID_Auth_data page;
        } auth;
        struct {
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
            float timestamp;
        } location;
        struct {
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
        } system;
        struct {
            bool has_operator_position;
            double operator_latitude;
            double operator_longitude;
            float operator_altitude_geo_m;
        } system_update;
    } data;
} remoteid_store_update_t;

// Why the device is (not) ready to broadcast / arm. Lets callers surface the
// specific reason (e.g. an invalid serial) instead of a generic "not ready".
typedef enum remoteid_health {
    REMOTEID_HEALTH_OK = 0,
    REMOTEID_HEALTH_WAITING_BASIC_ID,
    REMOTEID_HEALTH_WAITING_OPERATOR_ID,
    REMOTEID_HEALTH_INVALID_SERIAL,
    REMOTEID_HEALTH_SELFTEST_FAILED,
} remoteid_health_t;

esp_err_t remoteid_store_start(const remoteid_state_t *initial_state);
esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout);
esp_err_t remoteid_store_get_snapshot(remoteid_state_t *out);
esp_err_t remoteid_store_wait_ready(TickType_t timeout);
bool remoteid_store_is_ready(void);

// Current readiness health (the reason behind remoteid_store_is_ready()).
remoteid_health_t remoteid_store_health(void);
