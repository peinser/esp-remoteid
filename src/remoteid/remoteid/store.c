#include "store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define REMOTEID_STORE_READY_BIT BIT0
#define REMOTEID_STORE_QUEUE_LEN 8

static const char *TAG = "remoteid_store";

static QueueHandle_t s_update_queue;
static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_events;
static remoteid_state_t s_state;
static bool s_basic_id_valid;
static bool s_operator_id_valid;

static bool usable_text_field(const char *value, const char *placeholder)
{
    return value[0] != '\0' && strncmp(value, placeholder, ODID_ID_SIZE) != 0;
}

static bool basic_id_is_usable(const remoteid_state_t *state)
{
    return usable_text_field(state->uas_id, "CHANGE_ME_UAS_ID");
}

static bool operator_id_is_usable(const remoteid_state_t *state)
{
    return usable_text_field(state->operator_id, "CHANGE_ME_OP_ID");
}

static void update_ready_state_locked(void)
{
    bool ready = s_basic_id_valid && s_operator_id_valid;

    if (ready) {
        xEventGroupSetBits(s_events, REMOTEID_STORE_READY_BIT);
    } else {
        xEventGroupClearBits(s_events, REMOTEID_STORE_READY_BIT);
    }
}

static void log_missing_identity_locked(void)
{
    if (!s_basic_id_valid) {
        ESP_LOGW(TAG, "Remote ID broadcast waiting for valid Basic ID/UAS ID");
    }
    if (!s_operator_id_valid) {
        ESP_LOGW(TAG, "Remote ID broadcast waiting for valid Operator ID");
    }
}

static void apply_update_locked(const remoteid_store_update_t *update)
{
    switch (update->type) {
    case REMOTEID_STORE_UPDATE_BASIC_ID:
        strlcpy(s_state.uas_id, update->data.basic_id.uas_id, sizeof(s_state.uas_id));
        s_state.id_type = update->data.basic_id.id_type;
        s_state.ua_type = update->data.basic_id.ua_type;
        s_basic_id_valid = basic_id_is_usable(&s_state);
        ESP_LOGI(TAG, "updated Basic ID from input source (valid=%d)", s_basic_id_valid);
        break;
    case REMOTEID_STORE_UPDATE_OPERATOR_ID:
        strlcpy(s_state.operator_id, update->data.operator_id.operator_id, sizeof(s_state.operator_id));
        s_operator_id_valid = operator_id_is_usable(&s_state);
        ESP_LOGI(TAG, "updated Operator ID from input source (valid=%d)", s_operator_id_valid);
        break;
    case REMOTEID_STORE_UPDATE_LOCATION:
        s_state.has_position      = update->data.location.has_position;
        s_state.latitude          = update->data.location.latitude;
        s_state.longitude         = update->data.location.longitude;
        s_state.altitude_geo_m    = update->data.location.altitude_geo_m;
        s_state.altitude_baro_m   = update->data.location.altitude_baro_m;
        s_state.status            = update->data.location.status;
        s_state.speed_horizontal  = update->data.location.speed_horizontal;
        s_state.speed_vertical    = update->data.location.speed_vertical;
        s_state.direction         = update->data.location.direction;
        s_state.height            = update->data.location.height;
        s_state.height_type       = update->data.location.height_type;
        s_state.horiz_acc         = update->data.location.horiz_acc;
        s_state.vert_acc          = update->data.location.vert_acc;
        s_state.baro_acc          = update->data.location.baro_acc;
        s_state.speed_acc         = update->data.location.speed_acc;
        s_state.ts_acc            = update->data.location.ts_acc;
        s_state.timestamp         = update->data.location.timestamp;
        ESP_LOGD(TAG, "updated Location from input source (has_position=%d status=%d)",
                 s_state.has_position, s_state.status);
        break;
    case REMOTEID_STORE_UPDATE_SYSTEM:
        s_state.has_operator_position    = update->data.system.has_operator_position;
        s_state.operator_latitude        = update->data.system.operator_latitude;
        s_state.operator_longitude       = update->data.system.operator_longitude;
        s_state.operator_altitude_geo_m  = update->data.system.operator_altitude_geo_m;
        s_state.operator_location_type   = update->data.system.operator_location_type;
        s_state.area_count               = update->data.system.area_count;
        s_state.area_radius              = update->data.system.area_radius;
        s_state.area_ceiling_m           = update->data.system.area_ceiling_m;
        s_state.area_floor_m             = update->data.system.area_floor_m;
        s_state.classification_type      = update->data.system.classification_type;
        s_state.eu_category              = update->data.system.eu_category;
        s_state.eu_class                 = update->data.system.eu_class;
        ESP_LOGD(TAG, "updated System from input source");
        break;
    case REMOTEID_STORE_UPDATE_TAKEOFF:
        if (s_state.has_position) {
            s_state.has_operator_position   = true;
            s_state.operator_latitude       = s_state.latitude;
            s_state.operator_longitude      = s_state.longitude;
            s_state.operator_altitude_geo_m = s_state.altitude_geo_m;
            s_state.operator_location_type  = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
            ESP_LOGI(TAG, "takeoff position set: %.6f, %.6f @ %.1f m",
                     s_state.operator_latitude, s_state.operator_longitude,
                     s_state.operator_altitude_geo_m);
        } else {
            ESP_LOGW(TAG, "arm detected but no GPS fix — takeoff position not captured");
        }
        break;
    }

    bool was_ready = (xEventGroupGetBits(s_events) & REMOTEID_STORE_READY_BIT) != 0;
    update_ready_state_locked();
    bool is_ready = s_basic_id_valid && s_operator_id_valid;
    if (!was_ready && is_ready) {
        ESP_LOGI(TAG, "Remote ID identity complete, broadcasts may start");
    }
}

static void remoteid_store_task(void *arg)
{
    (void)arg;
    remoteid_store_update_t update;

    while (true) {
        if (xQueueReceive(s_update_queue, &update, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        apply_update_locked(&update);
        xSemaphoreGive(s_state_mutex);
    }
}

esp_err_t remoteid_store_start(const remoteid_state_t *initial_state)
{
    ESP_RETURN_ON_FALSE(initial_state, ESP_ERR_INVALID_ARG, TAG, "initial state is required");

    s_update_queue = xQueueCreate(REMOTEID_STORE_QUEUE_LEN, sizeof(remoteid_store_update_t));
    ESP_RETURN_ON_FALSE(s_update_queue, ESP_ERR_NO_MEM, TAG, "create state update queue");

    s_state_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_mutex, ESP_ERR_NO_MEM, TAG, "create state mutex");

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "create state event group");

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = *initial_state;
    s_basic_id_valid = basic_id_is_usable(&s_state);
    s_operator_id_valid = operator_id_is_usable(&s_state);
    update_ready_state_locked();
    if (s_basic_id_valid && s_operator_id_valid) {
        ESP_LOGI(TAG, "Remote ID identity loaded from Kconfig, broadcasts may start");
    } else {
        log_missing_identity_locked();
    }
    xSemaphoreGive(s_state_mutex);

    if (xTaskCreate(remoteid_store_task, "remoteid_store", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create remoteid_store task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t remoteid_store_submit(const remoteid_store_update_t *update, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(update, ESP_ERR_INVALID_ARG, TAG, "update is required");
    ESP_RETURN_ON_FALSE(s_update_queue, ESP_ERR_INVALID_STATE, TAG, "state store is not started");

    if (xQueueSend(s_update_queue, update, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t remoteid_store_get_snapshot(remoteid_state_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "snapshot destination is required");
    ESP_RETURN_ON_FALSE(s_state_mutex, ESP_ERR_INVALID_STATE, TAG, "state store is not started");

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_state_mutex);

    return ESP_OK;
}

esp_err_t remoteid_store_wait_ready(TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "state store is not started");

    EventBits_t bits = xEventGroupWaitBits(s_events, REMOTEID_STORE_READY_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & REMOTEID_STORE_READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool remoteid_store_is_ready(void)
{
    if (!s_events) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & REMOTEID_STORE_READY_BIT) != 0;
}
