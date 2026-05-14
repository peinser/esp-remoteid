#include "encode.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "opendroneid.h"

static const char *TAG = "remoteid_encode";

static esp_err_t check_odid_encode(int rc, const char *message_name)
{
    if (rc == ODID_SUCCESS) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "failed to encode %s OpenDroneID message", message_name);
    return ESP_FAIL;
}

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void fill_basic_id_data(const remoteid_state_t *state, ODID_BasicID_data *data)
{
    data->IDType = state->id_type;
    data->UAType = state->ua_type;
    strncpy(data->UASID, state->uas_id, sizeof(data->UASID) - 1);
}

static void fill_location_data(const remoteid_state_t *state, ODID_Location_data *data)
{
    data->Status          = state->status;
    data->Direction       = state->direction;
    data->SpeedHorizontal = state->speed_horizontal;
    data->SpeedVertical   = state->speed_vertical;
    data->Latitude        = state->latitude;
    data->Longitude       = state->longitude;
    data->AltitudeBaro    = state->altitude_baro_m;
    data->AltitudeGeo     = state->altitude_geo_m;
    data->HeightType      = state->height_type;
    data->Height          = state->height;
    data->HorizAccuracy   = state->horiz_acc;
    data->VertAccuracy    = state->vert_acc;
    data->BaroAccuracy    = state->baro_acc;
    data->SpeedAccuracy   = state->speed_acc;
    data->TSAccuracy      = state->ts_acc;
    data->TimeStamp       = state->timestamp >= 0.0f
                            ? state->timestamp
                            : (float)(uptime_seconds() % MAX_TIMESTAMP);
}

static void fill_system_data(const remoteid_state_t *state, ODID_System_data *data)
{
    data->OperatorLocationType = state->operator_location_type;
    data->ClassificationType   = state->classification_type;
    data->OperatorLatitude     = state->has_operator_position ? state->operator_latitude  : 0.0;
    data->OperatorLongitude    = state->has_operator_position ? state->operator_longitude : 0.0;
    data->AreaCount            = state->area_count;
    data->AreaRadius           = state->area_radius;
    data->AreaCeiling          = state->area_ceiling_m;
    data->AreaFloor            = state->area_floor_m;
    data->CategoryEU           = state->eu_category;
    data->ClassEU              = state->eu_class;
    data->OperatorAltitudeGeo  = state->has_operator_position
                                 ? state->operator_altitude_geo_m
                                 : (float)INV_ALT;
    data->Timestamp            = uptime_seconds();
}

static void fill_operator_id_data(const remoteid_state_t *state, ODID_OperatorID_data *data)
{
    data->OperatorIdType = ODID_OPERATOR_ID;
    strncpy(data->OperatorId, state->operator_id, sizeof(data->OperatorId) - 1);
}

static esp_err_t build_basic_id(const remoteid_state_t *state, uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_BasicID_data data;
    ODID_BasicID_encoded encoded;

    odid_initBasicIDData(&data);
    fill_basic_id_data(state, &data);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeBasicIDMessage(&encoded, &data), "Basic ID"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_location(const remoteid_state_t *state, uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_Location_data data;
    ODID_Location_encoded encoded;

    odid_initLocationData(&data);
    fill_location_data(state, &data);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeLocationMessage(&encoded, &data), "Location"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_system(const remoteid_state_t *state, uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_System_data data;
    ODID_System_encoded encoded;

    odid_initSystemData(&data);
    fill_system_data(state, &data);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeSystemMessage(&encoded, &data), "System"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

static esp_err_t build_operator_id(const remoteid_state_t *state, uint8_t out[ODID_MESSAGE_SIZE])
{
    ODID_OperatorID_data data;
    ODID_OperatorID_encoded encoded;

    odid_initOperatorIDData(&data);
    fill_operator_id_data(state, &data);

    ESP_RETURN_ON_ERROR(check_odid_encode(encodeOperatorIDMessage(&encoded, &data), "Operator ID"), TAG, "");
    memcpy(out, &encoded, ODID_MESSAGE_SIZE);

    return ESP_OK;
}

esp_err_t remoteid_encode_uas_data(const remoteid_state_t *state, ODID_UAS_Data *uas_data)
{
    odid_initUasData(uas_data);
    fill_basic_id_data(state, &uas_data->BasicID[0]);
    fill_location_data(state, &uas_data->Location);
    fill_system_data(state, &uas_data->System);
    fill_operator_id_data(state, &uas_data->OperatorID);

    uas_data->BasicIDValid[0] = 1;
    uas_data->LocationValid = 1;
    uas_data->SystemValid = 1;
    uas_data->OperatorIDValid = 1;

    return ESP_OK;
}

esp_err_t remoteid_encode_static_messages(const remoteid_state_t *state, remoteid_message_bundle_t *bundle)
{
    ESP_RETURN_ON_ERROR(build_basic_id(state, bundle->messages[REMOTEID_MESSAGE_BASIC_ID]), TAG, "");
    ESP_RETURN_ON_ERROR(build_operator_id(state, bundle->messages[REMOTEID_MESSAGE_OPERATOR_ID]), TAG, "");

    return ESP_OK;
}

esp_err_t remoteid_encode_dynamic_message(const remoteid_state_t *state, remoteid_message_bundle_t *bundle,
                                          remoteid_message_index_t message_index)
{
    if (message_index == REMOTEID_MESSAGE_LOCATION) {
        return build_location(state, bundle->messages[REMOTEID_MESSAGE_LOCATION]);
    }
    if (message_index == REMOTEID_MESSAGE_SYSTEM) {
        return build_system(state, bundle->messages[REMOTEID_MESSAGE_SYSTEM]);
    }

    return ESP_OK;
}
