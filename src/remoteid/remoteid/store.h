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
    // Self-test failed: latch a Remote ID system-failure. Carries no payload.
    REMOTEID_STORE_UPDATE_SELFTEST_FAILED,
    // Autopilot emergency state, derived from HEARTBEAT system_status. Latched
    // unless CONFIG_REMOTEID_MAVLINK_EMERGENCY_LATCH is disabled.
    REMOTEID_STORE_UPDATE_EMERGENCY,
    REMOTEID_STORE_UPDATE_SELF_ID,
    // A single received Authentication page. The store accumulates pages and
    // marks external auth ready once 0..LastPageIndex have all arrived.
    REMOTEID_STORE_UPDATE_AUTH,
    // Vehicle armed: record the current location as the take-off point and use
    // it as the operator position unless a source supplies a live or fixed one.
    // Carries no payload. Ignored when the location is stale.
    REMOTEID_STORE_UPDATE_TAKEOFF,
} remoteid_store_update_type_t;

typedef struct remoteid_store_update {
    remoteid_store_update_type_t type;
    union {
        struct {
            char uas_id[ODID_ID_SIZE + 1]; // 20 raw bytes, see remoteid_state_t
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
            float timestamp;                // seconds after the UTC hour, < 0 if unknown
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
            uint32_t timestamp;             // source time, seconds since 2019-01-01 UTC
        } system;
        struct {
            bool has_operator_position;
            double operator_latitude;
            double operator_longitude;
            float operator_altitude_geo_m;
            uint32_t timestamp;             // source time, seconds since 2019-01-01 UTC
        } system_update;
        struct {
            bool active;
        } emergency;
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
    // Transports not started yet, or a transport/runtime fault not yet latched.
    REMOTEID_HEALTH_TRANSPORTS_NOT_RUNNING,
    // A runtime position source is configured but no fresh Location arrived.
    REMOTEID_HEALTH_LOCATION_STALE,
    // An operator ID failed the consistency check (EU 2019/945: the operator
    // gets an error message). Cleared by the next valid operator ID.
    REMOTEID_HEALTH_OPERATOR_ID_REJECTED,
    // Strict mode: the fresh Location lacks the mandatory position, height or time.
    REMOTEID_HEALTH_LOCATION_INCOMPLETE,
} remoteid_health_t;

esp_err_t remoteid_store_start(const remoteid_state_t *initial_state);
esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout);
esp_err_t remoteid_store_get_snapshot(remoteid_state_t *out);
esp_err_t remoteid_store_wait_ready(TickType_t timeout);
bool remoteid_store_is_ready(void);

// Current readiness health (the reason behind remoteid_store_is_ready()).
remoteid_health_t remoteid_store_health(void);

// Pre-arm failure text reported to the autopilot (OPEN_DRONE_ID_ARM_STATUS /
// dronecan.remoteid.ArmStatus); "" when the health is OK.
const char *remoteid_store_health_text(remoteid_health_t health);
