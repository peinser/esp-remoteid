#include "model.h"

#include <string.h>

#include "config.h"

static ODID_idtype_t configured_id_type(void)
{
#if CONFIG_REMOTEID_ID_TYPE_SERIAL_NUMBER
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

esp_err_t remoteid_model_init(remoteid_state_t *state)
{
    memset(state, 0, sizeof(*state));

    strncpy(state->uas_id, REMOTEID_UAS_ID, sizeof(state->uas_id) - 1);
    strncpy(state->operator_id, REMOTEID_OPERATOR_ID, sizeof(state->operator_id) - 1);
    state->id_type = configured_id_type();
    state->ua_type = configured_ua_type();

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

    state->operator_location_type = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    state->area_count             = 1;
    state->area_radius            = 0;
    state->area_ceiling_m         = (float)INV_ALT;
    state->area_floor_m           = (float)INV_ALT;
    state->classification_type    = ODID_CLASSIFICATION_TYPE_EU;
    state->eu_category            = configured_eu_category();
    state->eu_class               = configured_eu_class();

#if CONFIG_REMOTEID_HAS_POSITION
    state->has_position          = true;
    state->latitude              = REMOTEID_TAKEOFF_LATITUDE;
    state->longitude             = REMOTEID_TAKEOFF_LONGITUDE;
    state->altitude_geo_m        = REMOTEID_TAKEOFF_ALTITUDE_M;
    state->altitude_baro_m       = REMOTEID_TAKEOFF_ALTITUDE_M;
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
