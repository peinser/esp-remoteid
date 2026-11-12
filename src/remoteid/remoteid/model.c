#include "model.h"

#include <string.h>

#include "config.h"

// The UAS ID and UA type are configured, not taken from input links, with a
// static identity and in strict compliance mode (EU 2019/945: the serial number
// cannot be modified by the user).
#define REMOTEID_CONFIGURED_UAS_ID (CONFIG_REMOTEID_IDENTITY_STATIC || CONFIG_REMOTEID_COMPLIANCE_STRICT)

#if CONFIG_REMOTEID_COMPLIANCE_STRICT

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"

static const char *TAG = "remoteid_model";

// EN 4709-002: the serial is loaded at the factory, cannot be modified afterwards
// and is stored in secure memory. It lives in the one-time-programmable eFuse
// user data block (BLOCK_USR_DATA, first 20 bytes, NUL-padded ASCII), burned and
// write-protected at production (see the README). A firmware image carries no
// serial, so one signed image serves every unit.
static void read_serial_from_efuse(char uas_id[ODID_ID_SIZE + 1])
{
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, uas_id, ODID_ID_SIZE * 8) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read the serial number from eFuse");
        memset(uas_id, 0, ODID_ID_SIZE + 1);
        return;
    }
    uas_id[ODID_ID_SIZE] = '\0';
    if (uas_id[0] == '\0') {
        ESP_LOGE(TAG, "no serial number in eFuse BLOCK_USR_DATA: the unit is not provisioned");
    } else if (!esp_efuse_read_field_bit(ESP_EFUSE_WR_DIS_BLOCK_USR_DATA)) {
        ESP_LOGW(TAG, "eFuse BLOCK_USR_DATA is not write-protected: the serial number can still be altered");
    }
}

#endif /* CONFIG_REMOTEID_COMPLIANCE_STRICT */

#if REMOTEID_CONFIGURED_UAS_ID

static ODID_idtype_t configured_id_type(void)
{
#if CONFIG_REMOTEID_COMPLIANCE_STRICT || CONFIG_REMOTEID_ID_TYPE_SERIAL_NUMBER
    return ODID_IDTYPE_SERIAL_NUMBER;
#else
    return ODID_IDTYPE_CAA_REGISTRATION_ID;
#endif
}

static ODID_uatype_t configured_ua_type(void)
{
#if CONFIG_REMOTEID_UA_TYPE_AEROPLANE
    return ODID_UATYPE_AEROPLANE;
#elif CONFIG_REMOTEID_UA_TYPE_HYBRID_LIFT
    return ODID_UATYPE_HYBRID_LIFT;
#elif CONFIG_REMOTEID_UA_TYPE_OTHER
    return ODID_UATYPE_OTHER;
#else
    return ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
#endif
}

#endif /* REMOTEID_CONFIGURED_UAS_ID */

#if CONFIG_REMOTEID_IDENTITY_STATIC

static ODID_category_EU_t configured_eu_category(void)
{
#if CONFIG_REMOTEID_EU_CATEGORY_UNDECLARED
    return ODID_CATEGORY_EU_UNDECLARED;
#elif CONFIG_REMOTEID_EU_CATEGORY_SPECIFIC
    return ODID_CATEGORY_EU_SPECIFIC;
#elif CONFIG_REMOTEID_EU_CATEGORY_CERTIFIED
    return ODID_CATEGORY_EU_CERTIFIED;
#else
    return ODID_CATEGORY_EU_OPEN;
#endif
}

static ODID_class_EU_t configured_eu_class(void)
{
#if CONFIG_REMOTEID_EU_CLASS_C0
    return ODID_CLASS_EU_CLASS_0;
#elif CONFIG_REMOTEID_EU_CLASS_C1
    return ODID_CLASS_EU_CLASS_1;
#elif CONFIG_REMOTEID_EU_CLASS_C2
    return ODID_CLASS_EU_CLASS_2;
#elif CONFIG_REMOTEID_EU_CLASS_C3
    return ODID_CLASS_EU_CLASS_3;
#elif CONFIG_REMOTEID_EU_CLASS_C4
    return ODID_CLASS_EU_CLASS_4;
#elif CONFIG_REMOTEID_EU_CLASS_C5
    return ODID_CLASS_EU_CLASS_5;
#elif CONFIG_REMOTEID_EU_CLASS_C6
    return ODID_CLASS_EU_CLASS_6;
#else
    return ODID_CLASS_EU_UNDECLARED;
#endif
}

#endif /* CONFIG_REMOTEID_IDENTITY_STATIC */

esp_err_t remoteid_model_init(remoteid_state_t *state)
{
    memset(state, 0, sizeof(*state));

    // Runtime identity: whatever is not configured stays empty and undeclared
    // until a flight controller or GCS supplies it over MAVLink/DroneCAN. The
    // readiness gate keeps the module from broadcasting until a valid Basic ID
    // and Operator ID are present. (memset already cleared uas_id/operator_id.)
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    read_serial_from_efuse(state->uas_id);
#elif CONFIG_REMOTEID_IDENTITY_STATIC
    strlcpy(state->uas_id, REMOTEID_UAS_ID, sizeof(state->uas_id));
#endif
#if REMOTEID_CONFIGURED_UAS_ID
    state->id_type     = configured_id_type();
    state->ua_type     = configured_ua_type();
#else
    state->id_type     = ODID_IDTYPE_NONE;
    state->ua_type     = ODID_UATYPE_NONE;
#endif
#if CONFIG_REMOTEID_IDENTITY_STATIC
    strlcpy(state->operator_id, REMOTEID_OPERATOR_ID, sizeof(state->operator_id));
    state->eu_category = configured_eu_category();
    state->eu_class    = configured_eu_class();
#else
    state->eu_category = ODID_CATEGORY_EU_UNDECLARED;
    state->eu_class    = ODID_CLASS_EU_UNDECLARED;
#endif

    state->speed_horizontal = (float)INV_SPEED_H;
    state->speed_vertical   = (float)INV_SPEED_V;
    state->direction        = (float)INV_DIR;
    state->height           = (float)INV_ALT;
    state->height_type      = ODID_HEIGHT_REF_OVER_TAKEOFF;
    state->horiz_acc        = ODID_HOR_ACC_UNKNOWN;
    state->vert_acc         = ODID_VER_ACC_UNKNOWN;
    state->baro_acc         = ODID_VER_ACC_UNKNOWN;
    state->speed_acc        = ODID_SPEED_ACC_UNKNOWN;
    state->ts_acc           = ODID_TIME_ACC_UNKNOWN;
    state->timestamp        = -1.0f;
    state->location_sample_timestamp = -1.0f;

    state->operator_location_type = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    state->area_count             = 1;
    state->area_radius            = 0;
    state->area_ceiling_m         = (float)INV_ALT;
    state->area_floor_m           = (float)INV_ALT;
    state->classification_type    = ODID_CLASSIFICATION_TYPE_EU;

#if CONFIG_REMOTEID_HAS_POSITION
    state->has_position          = true;
    state->latitude              = REMOTEID_TAKEOFF_LATITUDE;
    state->longitude             = REMOTEID_TAKEOFF_LONGITUDE;
    state->altitude_geo_m        = REMOTEID_TAKEOFF_ALTITUDE_M;
    // The configured altitude is WGS-84 (geodetic); there is no pressure altitude.
    state->altitude_baro_m       = (float)INV_ALT;
    state->status                = ODID_STATUS_AIRBORNE;
    state->has_operator_position = true;
    state->operator_latitude     = REMOTEID_TAKEOFF_LATITUDE;
    state->operator_longitude    = REMOTEID_TAKEOFF_LONGITUDE;
    state->operator_altitude_geo_m = REMOTEID_TAKEOFF_ALTITUDE_M;
#else
    state->has_position          = false;
    state->latitude              = 0.0;
    state->longitude             = 0.0;
    state->altitude_geo_m        = (float)INV_ALT;
    state->altitude_baro_m       = (float)INV_ALT;
    state->status                = ODID_STATUS_UNDECLARED;
    state->has_operator_position = false;
    state->operator_latitude     = 0.0;
    state->operator_longitude    = 0.0;
    state->operator_altitude_geo_m = (float)INV_ALT;
#endif

    return ESP_OK;
}
