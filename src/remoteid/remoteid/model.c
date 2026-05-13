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
    state->eu_category = configured_eu_category();
    state->eu_class = configured_eu_class();

#if CONFIG_REMOTEID_HAS_POSITION
    state->has_position = true;
    state->latitude = REMOTEID_TAKEOFF_LATITUDE;
    state->longitude = REMOTEID_TAKEOFF_LONGITUDE;
    state->altitude_m = REMOTEID_TAKEOFF_ALTITUDE_M;
#else
    state->has_position = false;
    state->latitude = 0.0;
    state->longitude = 0.0;
    state->altitude_m = (float)INV_ALT;
#endif

    return ESP_OK;
}
