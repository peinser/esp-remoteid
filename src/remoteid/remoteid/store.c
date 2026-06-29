#include "store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "indicator.h"
#include "lighting.h"

#ifndef CONFIG_REMOTEID_COMPLIANCE_STRICT
#define CONFIG_REMOTEID_COMPLIANCE_STRICT 0
#endif

#define REMOTEID_STORE_READY_BIT BIT0
#define REMOTEID_STORE_QUEUE_LEN 8

static const char *TAG = "remoteid_store";

static QueueHandle_t s_update_queue;
static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_events;
static remoteid_state_t s_state;
static bool s_basic_id_valid;
static bool s_operator_id_valid;
// Set when a serial-typed UAS ID failed ANSI/CTA-2063-A validation, so the health
// reason stays specific even after a bad serial is cleared in compliance mode.
static bool s_serial_invalid;
// Latched when the self-test fails: broadcast status is forced to system-failure
// and arming is blocked.
static bool s_selftest_failed;
// Live autopilot emergency state (from HEARTBEAT). Forces EMERGENCY status unless a
// system-failure is latched; clears when the autopilot recovers.
static bool s_autopilot_emergency;

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

static bool serial_char_ok(char c)
{
    // Alphanumeric. CTA-2063-A excludes 'O'/'I' in the serial body, but we accept
    // the full alnum set to avoid false rejects of validly-assigned codes.
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Validate the ANSI/CTA-2063-A structure: 4-char manufacturer code, a 1-char hex
// length code (1..F), then exactly that many serial characters.
static bool serial_is_valid_cta2063a(const char *s)
{
    size_t len = strlen(s);
    if (len < 6) {
        return false; // 4 (mfr) + 1 (length code) + >= 1 (serial)
    }
    for (size_t i = 0; i < 4; i++) {
        if (!serial_char_ok(s[i])) {
            return false;
        }
    }
    char lc = s[4];
    int serial_len;
    if (lc >= '1' && lc <= '9')      serial_len = lc - '0';
    else if (lc >= 'A' && lc <= 'F') serial_len = lc - 'A' + 10;
    else if (lc >= 'a' && lc <= 'f') serial_len = lc - 'a' + 10;
    else return false;

    if (len != (size_t)(5 + serial_len)) {
        return false; // declared length must match the actual serial length
    }
    for (size_t i = 5; i < len; i++) {
        if (!serial_char_ok(s[i])) {
            return false;
        }
    }
    return true;
}

// Recompute Basic ID validity, including the CTA-2063-A serial check. In strict
// compliance mode a malformed serial is cleared (never broadcast) and raises the
// error indicators; otherwise it is only logged.
static void apply_basic_id_validity_locked(void)
{
    s_serial_invalid = false;

    if (s_state.id_type == ODID_IDTYPE_SERIAL_NUMBER &&
        s_state.uas_id[0] != '\0' &&
        !serial_is_valid_cta2063a(s_state.uas_id)) {
        ESP_LOGW(TAG, "UAS serial '%s' is not valid ANSI/CTA-2063-A", s_state.uas_id);
        s_serial_invalid = true;
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
        ESP_LOGE(TAG, "compliance mode: rejecting invalid serial, refusing to broadcast");
        s_state.uas_id[0] = '\0'; // clear so a malformed serial is never broadcast
        indicator_set_error();
        lighting_set_error();
#endif
    }

    s_basic_id_valid = basic_id_is_usable(&s_state);
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
        apply_basic_id_validity_locked();
        ESP_LOGI(TAG, "updated Basic ID from input source (valid=%d, serial_invalid=%d)",
                 s_basic_id_valid, s_serial_invalid);
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
        // Status precedence: a latched RID system-failure first, then a live
        // autopilot emergency, otherwise whatever the LOCATION source reported.
        s_state.status            = s_selftest_failed    ? ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE
                                  : s_autopilot_emergency ? ODID_STATUS_EMERGENCY
                                                          : update->data.location.status;
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
        if (s_state.has_operator_position) {
            s_state.operator_position_updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        ESP_LOGD(TAG, "updated System from input source");
        break;
    case REMOTEID_STORE_UPDATE_SYSTEM_UPDATE:
        s_state.has_operator_position    = update->data.system_update.has_operator_position;
        s_state.operator_latitude        = update->data.system_update.operator_latitude;
        s_state.operator_longitude       = update->data.system_update.operator_longitude;
        s_state.operator_altitude_geo_m  = update->data.system_update.operator_altitude_geo_m;
        if (s_state.has_operator_position) {
            s_state.operator_position_updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        ESP_LOGD(TAG, "updated operator position from System Update (has_position=%d)",
                 s_state.has_operator_position);
        break;
    case REMOTEID_STORE_UPDATE_OPERATOR_LOCATION_EXPIRE:
        s_state.has_operator_position = false;
        ESP_LOGW(TAG, "operator position expired and cleared");
        break;
    case REMOTEID_STORE_UPDATE_SELFTEST_FAILED:
        s_selftest_failed = true;
        s_state.status = ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE;
        ESP_LOGE(TAG, "self-test failed: broadcasting Remote ID system-failure status");
        break;
    case REMOTEID_STORE_UPDATE_EMERGENCY:
        s_autopilot_emergency = update->data.emergency.active;
        // Reflect an asserted emergency immediately; on clear the next Location
        // update restores the reported ground/airborne status. System-failure,
        // being a latched RID fault, keeps precedence.
        if (s_autopilot_emergency && !s_selftest_failed) {
            s_state.status = ODID_STATUS_EMERGENCY;
        }
        ESP_LOGW(TAG, "autopilot emergency %s", s_autopilot_emergency ? "asserted" : "cleared");
        break;
    case REMOTEID_STORE_UPDATE_SELF_ID:
        s_state.self_id_type = update->data.self_id.desc_type;
        strlcpy(s_state.self_id_desc, update->data.self_id.desc, sizeof(s_state.self_id_desc));
        s_state.has_self_id = true;
        ESP_LOGI(TAG, "updated Self ID from input source (type=%d)", s_state.self_id_type);
        break;
    case REMOTEID_STORE_UPDATE_AUTH: {
        const ODID_Auth_data *page = &update->data.auth.page;
        if (page->DataPage >= ODID_AUTH_MAX_PAGES) {
            ESP_LOGW(TAG, "ignored Authentication page %u beyond max %d", page->DataPage, ODID_AUTH_MAX_PAGES);
            break;
        }
        s_state.ext_auth_pages[page->DataPage] = *page;
        s_state.ext_auth_pages_received |= (uint16_t)(1u << page->DataPage);
        if (page->DataPage == 0) {
            s_state.ext_auth_last_page = page->LastPageIndex;
        }
        // External auth is broadcastable only once page 0 (which carries
        // LastPageIndex) and every page up to it have been received.
        if (s_state.ext_auth_pages_received & 1u) {
            uint16_t needed = (uint16_t)((1u << (s_state.ext_auth_last_page + 1)) - 1);
            s_state.has_ext_auth = (s_state.ext_auth_pages_received & needed) == needed;
        }
        ESP_LOGD(TAG, "stored Authentication page %u/%u (ready=%d)",
                 page->DataPage, s_state.ext_auth_last_page, s_state.has_ext_auth);
        break;
    }
    case REMOTEID_STORE_UPDATE_TAKEOFF:
        if (s_state.has_position) {
            s_state.has_operator_position   = true;
            s_state.operator_latitude       = s_state.latitude;
            s_state.operator_longitude      = s_state.longitude;
            s_state.operator_altitude_geo_m = s_state.altitude_geo_m;
            s_state.operator_location_type  = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
            s_state.operator_position_updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG, "takeoff position set: %.6f, %.6f @ %.1f m",
                     s_state.operator_latitude, s_state.operator_longitude,
                     s_state.operator_altitude_geo_m);
        } else {
            ESP_LOGW(TAG, "arm detected but no GPS fix, takeoff position not captured");
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
    apply_basic_id_validity_locked();
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

remoteid_health_t remoteid_store_health(void)
{
    if (!s_state_mutex) {
        return REMOTEID_HEALTH_WAITING_BASIC_ID;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    remoteid_health_t health;
    if (s_selftest_failed) {
        health = REMOTEID_HEALTH_SELFTEST_FAILED;  // most severe
    } else if (s_serial_invalid && !s_basic_id_valid) {
        health = REMOTEID_HEALTH_INVALID_SERIAL;   // specific reason beats generic
    } else if (!s_basic_id_valid) {
        health = REMOTEID_HEALTH_WAITING_BASIC_ID;
    } else if (!s_operator_id_valid) {
        health = REMOTEID_HEALTH_WAITING_OPERATOR_ID;
    } else {
        health = REMOTEID_HEALTH_OK;
    }
    xSemaphoreGive(s_state_mutex);
    return health;
}
