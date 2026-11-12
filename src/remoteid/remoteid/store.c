#include "store.h"

#include <ctype.h>
#include <string.h>

#include "encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "clock.h"
#include "indicator.h"
#include "lighting.h"
#include "selftest.h"

#ifndef CONFIG_REMOTEID_COMPLIANCE_STRICT
#define CONFIG_REMOTEID_COMPLIANCE_STRICT 0
#endif

// A runtime producer (MAVLink / DroneCAN) is the position source unless a fixed
// Kconfig position is configured. Only then can the aircraft position go stale.
#if (CONFIG_REMOTEID_MAVLINK_INPUT || CONFIG_REMOTEID_DRONECAN_INPUT) && !CONFIG_REMOTEID_HAS_POSITION
#define REMOTEID_RUNTIME_LOCATION 1
#else
#define REMOTEID_RUNTIME_LOCATION 0
#endif

// A Location older than this is stale: not ready to arm and not usable as a
// takeoff position. Location is a 1 Hz message, so 3 s allows two lost updates.
#define REMOTEID_LOCATION_FRESH_MS 3000

#ifndef CONFIG_REMOTEID_OPERATOR_LOCATION_EXPIRY_MS
#define CONFIG_REMOTEID_OPERATOR_LOCATION_EXPIRY_MS 10000
#endif

#define REMOTEID_STORE_READY_BIT BIT0
#define REMOTEID_STORE_QUEUE_LEN 8
#define REMOTEID_STORE_TICK_MS   1000

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
// Set when an operator ID failed the consistency check, until a valid one is
// accepted. EU 2019/945 requires an error message to the operator; it is
// reported through ARM_STATUS / ArmStatus and the error indicator.
static bool s_operator_id_rejected;
// Latched when the self-test fails: broadcast status is forced to system-failure
// and arming is blocked.
static bool s_selftest_failed;
// Autopilot emergency state (from HEARTBEAT). Forces EMERGENCY status unless a
// system-failure is latched.
static bool s_autopilot_emergency;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ID types 1 (serial) and 2 (CAA registration) are NUL-padded printable ASCII.
// Anything after the first NUL is padding and is cleared.
static bool normalize_text_field(char *value, size_t size)
{
    size_t len = strnlen(value, size);
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (value[i] < 0x20 || value[i] > 0x7e) {
            return false;
        }
    }
    memset(value + len, 0, size - len);
    return true;
}

// ID types 3 (UTM UUID) and 4 (specific session ID) are binary; NUL is a valid
// byte, so the only check is that some ID is present.
static bool binary_field_is_set(const char *value, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (value[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool normalize_uas_id(ODID_idtype_t id_type, char uas_id[ODID_ID_SIZE])
{
    switch (id_type) {
    case ODID_IDTYPE_SERIAL_NUMBER:
    case ODID_IDTYPE_CAA_REGISTRATION_ID:
        return normalize_text_field(uas_id, ODID_ID_SIZE);
    case ODID_IDTYPE_UTM_ASSIGNED_UUID:
    case ODID_IDTYPE_SPECIFIC_SESSION_ID:
        return binary_field_is_set(uas_id, ODID_ID_SIZE);
    default:
        return false; // NONE or a reserved type cannot identify the aircraft
    }
}

static int base36_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

// EU operator registration number check character: Luhn mod 36 over
// the 12 public characters after the country code followed by the 3 secret
// characters. Example: "FIN87astrdge12k8-xyz" -> check character '8'.
static bool eu_operator_checksum_ok(const char id[ODID_ID_SIZE])
{
    char input[15];
    memcpy(input, id + 3, 12);
    memcpy(input + 12, id + 17, 3);

    int factor = 2;
    int sum = 0;
    for (int i = 14; i >= 0; i--) {
        int value = base36_value(input[i]);
        if (value < 0) {
            return false;
        }
        int addend = value * factor;
        factor = (factor == 2) ? 1 : 2;
        sum += addend / 36 + addend % 36;
    }
    return base36_value(id[15]) == (36 - sum % 36) % 36;
}

static bool is_upper_alpha(char c)
{
    return c >= 'A' && c <= 'Z';
}

// Validate an operator ID. A full EU registration number entered with its
// secret suffix ("<16 public>-<3 secret>") is checksum-verified and the
// verification-only suffix is removed so it is never stored or broadcast. In
// strict compliance mode only that verifiable form is accepted.
static bool normalize_operator_id(char id[ODID_ID_SIZE + 1])
{
    if (!normalize_text_field(id, ODID_ID_SIZE)) {
        return false;
    }
    bool has_secret = strlen(id) == ODID_ID_SIZE && id[16] == '-';
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    if (!has_secret || !is_upper_alpha(id[0]) || !is_upper_alpha(id[1]) || !is_upper_alpha(id[2])) {
        ESP_LOGW(TAG, "rejected operator ID: strict mode needs the full EU registration number "
                      "with its 3 secret characters (<country><13 characters>-<3 secret>)");
        return false;
    }
#else
    if (!has_secret) {
        return true;
    }
#endif
    if (!eu_operator_checksum_ok(id)) {
        ESP_LOGW(TAG, "rejected EU operator ID: checksum does not match the secret characters");
        return false;
    }
    // EN 4709-002 format: upper-case country code, the rest lower case. The
    // check is case-insensitive, so normalise to the registered spelling.
    for (int i = 3; i < 16; i++) {
        id[i] = (char)tolower((unsigned char)id[i]);
    }
    memset(id + 16, 0, ODID_ID_SIZE - 16);
    return true;
}

// Stored IDs are already normalized for their type, so any set byte is an ID.
static bool basic_id_is_usable(const remoteid_state_t *state)
{
    return binary_field_is_set(state->uas_id, ODID_ID_SIZE) &&
           strncmp(state->uas_id, "CHANGE_ME_UAS_ID", ODID_ID_SIZE) != 0;
}

static bool operator_id_is_usable(const remoteid_state_t *state)
{
    return state->operator_id[0] != '\0' &&
           strncmp(state->operator_id, "CHANGE_ME_OP_ID", ODID_ID_SIZE) != 0;
}

// ANSI/CTA-2063-A characters: digits and upper-case letters except O and I.
static bool serial_char_ok(char c)
{
    return (c >= '0' && c <= '9') || (is_upper_alpha(c) && c != 'O' && c != 'I');
}

// Validate the ANSI/CTA-2063-A structure: 4-char manufacturer code, a 1-char
// upper-case hex length code (1..F), then exactly that many serial characters.
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
        memset(s_state.uas_id, 0, sizeof(s_state.uas_id)); // a malformed serial is never broadcast
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

static bool location_is_fresh_locked(void)
{
    return s_state.location_updated_ms != 0 &&
           now_ms() - s_state.location_updated_ms <= REMOTEID_LOCATION_FRESH_MS;
}

// EU 2019/945 data (iii): time stamp, position and height above the surface or
// take-off point. Speed and course may legitimately be unknown when stationary.
static bool location_is_complete_locked(void)
{
    return s_state.has_position && s_state.height > (float)INV_ALT && s_state.timestamp >= 0.0f;
}

// Age the operator position by its source timestamp, not by receipt: only a
// newer source timestamp counts as a refresh, so an autopilot that keeps
// relaying a cached SYSTEM / SYSTEM_UPDATE cannot keep a live location fresh.
// An unknown time (0) is never evidence of a fresh measurement: a live position
// without a source timestamp expires.
static void refresh_operator_position_age_locked(uint32_t source_timestamp)
{
    if (source_timestamp > s_state.operator_position_timestamp) {
        s_state.operator_position_timestamp  = source_timestamp;
        s_state.operator_position_updated_ms = now_ms();
    }
}

// A live (LIVE_GNSS) operator position whose source timestamp has not advanced
// within the expiry window is stale. It stays stored with its role; while it is
// expired the take-off point is broadcast instead (remoteid_operator_position).
// Run on every update and on the store's 1 s tick, so it also fires when every
// input has stopped.
static void expire_live_operator_position_locked(void)
{
    if (!s_state.has_operator_position || s_state.operator_position_expired ||
        s_state.operator_location_type != ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS ||
        now_ms() - s_state.operator_position_updated_ms <= CONFIG_REMOTEID_OPERATOR_LOCATION_EXPIRY_MS) {
        return;
    }
    s_state.operator_position_expired = true;
    ESP_LOGW(TAG, "live operator position stale, broadcasting %s",
             s_state.has_takeoff_position ? "the take-off point" : "no operator position");
}

// Whether a position from a System / SystemUpdate with this source timestamp
// may replace the stored one. A known timestamp older than the stored one is
// out of order and dropped. After an expiry only a newer known timestamp
// counts, so neither the cached sample nor an unknown time revives it.
static bool operator_position_update_allowed_locked(uint32_t source_timestamp)
{
    if (source_timestamp == 0) {
        return !s_state.operator_position_expired;
    }
    if (source_timestamp < s_state.operator_position_timestamp) {
        return false;
    }
    if (s_state.operator_position_expired && source_timestamp == s_state.operator_position_timestamp) {
        return false;
    }
    s_state.operator_position_expired = false;
    return true;
}

// Apply one update to s_state. Returns false when the update is rejected, in
// which case the caller restores the previous state.
static bool apply_update_locked(remoteid_store_update_t *update)
{
    switch (update->type) {
    case REMOTEID_STORE_UPDATE_BASIC_ID:
        if (update->data.basic_id.id_type == ODID_IDTYPE_NONE &&
            !binary_field_is_set(update->data.basic_id.uas_id, ODID_ID_SIZE)) {
            return true; // nothing provided yet (ArduPilot sends an empty Basic ID until it has one)
        }
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
        // EU 2019/945: the serial number of the Remote ID system cannot be
        // modified by the user, so it is only the one configured at build time.
        ESP_LOGD(TAG, "ignored Basic ID from input source: strict mode uses the configured serial");
        return true;
#endif
        if (!normalize_uas_id(update->data.basic_id.id_type, update->data.basic_id.uas_id)) {
            ESP_LOGW(TAG, "rejected Basic ID: ID type %d with an empty or malformed UAS ID",
                     update->data.basic_id.id_type);
            return false;
        }
        memcpy(s_state.uas_id, update->data.basic_id.uas_id, ODID_ID_SIZE);
        s_state.uas_id[ODID_ID_SIZE] = '\0';
        s_state.id_type = update->data.basic_id.id_type;
        s_state.ua_type = update->data.basic_id.ua_type;
        apply_basic_id_validity_locked();
        ESP_LOGI(TAG, "updated Basic ID from input source (valid=%d, serial_invalid=%d)",
                 s_basic_id_valid, s_serial_invalid);
        break;
    case REMOTEID_STORE_UPDATE_OPERATOR_ID:
        if (update->data.operator_id.operator_id[0] == '\0') {
            return true; // nothing provided yet (ArduPilot sends an empty Operator ID until it has one)
        }
        if (!normalize_operator_id(update->data.operator_id.operator_id)) {
            if (!s_operator_id_rejected) { // sources repeat it every few seconds: log once
                ESP_LOGE(TAG, "rejected Operator ID: failed the consistency check");
            }
            s_operator_id_rejected = true;
            return false;
        }
        s_operator_id_rejected = false;
        strlcpy(s_state.operator_id, update->data.operator_id.operator_id, sizeof(s_state.operator_id));
        s_operator_id_valid = operator_id_is_usable(&s_state);
        ESP_LOGI(TAG, "updated Operator ID from input source (valid=%d)", s_operator_id_valid);
        break;
    case REMOTEID_STORE_UPDATE_LOCATION: {
        float timestamp = update->data.location.timestamp;
        ODID_Timestamp_accuracy_t ts_acc = update->data.location.ts_acc;
        if (timestamp < 0.0f || timestamp > (float)MAX_TIMESTAMP) {
            // The source has no time for this sample: stamp it with the arrival
            // time when UTC is known, otherwise leave it unknown. Never invent UTC.
            timestamp = remoteid_clock_seconds_after_hour();
            ts_acc    = ODID_TIME_ACC_UNKNOWN;
        }
        s_state.has_position      = update->data.location.has_position;
        s_state.latitude          = update->data.location.latitude;
        s_state.longitude         = update->data.location.longitude;
        s_state.altitude_geo_m    = update->data.location.altitude_geo_m;
        s_state.altitude_baro_m   = update->data.location.altitude_baro_m;
        // Status precedence: a latched RID system-failure first, then an
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
        s_state.ts_acc            = ts_acc;
        s_state.timestamp         = timestamp;
        s_state.location_updated_ms = now_ms();
        ESP_LOGD(TAG, "updated Location from input source (has_position=%d status=%d)",
                 s_state.has_position, s_state.status);
        break;
    }
    case REMOTEID_STORE_UPDATE_SYSTEM:
        // A System without an operator position (0, 0) carries none: ArduPilot
        // sends that until the ground station provides one. It does not erase
        // the stored one.
        if (update->data.system.has_operator_position &&
            operator_position_update_allowed_locked(update->data.system.timestamp)) {
            s_state.has_operator_position   = true;
            s_state.operator_latitude       = update->data.system.operator_latitude;
            s_state.operator_longitude      = update->data.system.operator_longitude;
            s_state.operator_altitude_geo_m = update->data.system.operator_altitude_geo_m;
            s_state.operator_location_type  = update->data.system.operator_location_type;
            refresh_operator_position_age_locked(update->data.system.timestamp);
        }
        s_state.area_count               = update->data.system.area_count;
        s_state.area_radius              = update->data.system.area_radius;
        s_state.area_ceiling_m           = update->data.system.area_ceiling_m;
        s_state.area_floor_m             = update->data.system.area_floor_m;
        // An undeclared classification carries no category or class (again a
        // zero-filled System); it must not replace the configured EU values.
        if (update->data.system.classification_type != ODID_CLASSIFICATION_TYPE_UNDECLARED) {
            s_state.classification_type = update->data.system.classification_type;
            s_state.eu_category         = update->data.system.eu_category;
            s_state.eu_class            = update->data.system.eu_class;
        }
        ESP_LOGD(TAG, "updated System from input source");
        break;
    case REMOTEID_STORE_UPDATE_SYSTEM_UPDATE:
        // Position only: the role (live / fixed) stays the one from the last System.
        if (!update->data.system_update.has_operator_position ||
            !operator_position_update_allowed_locked(update->data.system_update.timestamp)) {
            break;
        }
        s_state.has_operator_position    = true;
        s_state.operator_latitude        = update->data.system_update.operator_latitude;
        s_state.operator_longitude       = update->data.system_update.operator_longitude;
        s_state.operator_altitude_geo_m  = update->data.system_update.operator_altitude_geo_m;
        refresh_operator_position_age_locked(update->data.system_update.timestamp);
        ESP_LOGD(TAG, "updated operator position from System Update (has_position=%d)",
                 s_state.has_operator_position);
        break;
    case REMOTEID_STORE_UPDATE_SELFTEST_FAILED:
        s_selftest_failed = true;
        s_state.status = ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE;
        ESP_LOGE(TAG, "self-test failed: broadcasting Remote ID system-failure status");
        break;
    case REMOTEID_STORE_UPDATE_EMERGENCY:
        if (update->data.emergency.active) {
            s_autopilot_emergency = true;
            // Reflect immediately; system-failure (a latched RID fault) still wins.
            if (!s_selftest_failed) {
                s_state.status = ODID_STATUS_EMERGENCY;
            }
            ESP_LOGW(TAG, "autopilot emergency asserted");
        } else {
#if CONFIG_REMOTEID_MAVLINK_EMERGENCY_LATCH
            // Latched (default): an emergency is never downgraded automatically;
            // a power cycle is required to clear it.
#else
            // Live tracking: clear; the next Location update restores the
            // reported ground/airborne status.
            s_autopilot_emergency = false;
            ESP_LOGW(TAG, "autopilot emergency cleared");
#endif
        }
        break;
    case REMOTEID_STORE_UPDATE_SELF_ID:
        s_state.self_id_type = update->data.self_id.desc_type;
        strlcpy(s_state.self_id_desc, update->data.self_id.desc, sizeof(s_state.self_id_desc));
        s_state.has_self_id = true;
        ESP_LOGI(TAG, "updated Self ID from input source (type=%d)", s_state.self_id_type);
        break;
    case REMOTEID_STORE_UPDATE_AUTH: {
        const ODID_Auth_data *page = &update->data.auth.page;
        ODID_Auth_encoded encoded;
        // The encoder bounds DataPage, LastPageIndex and Length to what can be broadcast.
        if (encodeAuthMessage(&encoded, page) != ODID_SUCCESS) {
            ESP_LOGW(TAG, "rejected Authentication page %u: outside the broadcastable range", page->DataPage);
            return false;
        }
        if (page->DataPage == 0) {
            // A different page 0 starts a new signature: drop the pages of the
            // previous one so pages of two signatures are never broadcast together.
            ODID_Auth_encoded previous;
            bool same_set = (s_state.ext_auth_pages_received & 1u) &&
                            encodeAuthMessage(&previous, &s_state.ext_auth_pages[0]) == ODID_SUCCESS &&
                            memcmp(&previous, &encoded, sizeof(encoded)) == 0;
            if (!same_set) {
                s_state.ext_auth_pages_received = 0;
            }
            s_state.ext_auth_last_page  = page->LastPageIndex;
            s_state.ext_auth_updated_ms = now_ms();
        }
        s_state.ext_auth_pages[page->DataPage] = *page;
        s_state.ext_auth_pages_received |= (uint16_t)(1u << page->DataPage);
        // Broadcastable only once page 0 (which carries LastPageIndex) and every
        // page up to it belong to the same signature.
        uint16_t needed = (uint16_t)((1u << (s_state.ext_auth_last_page + 1)) - 1);
        s_state.has_ext_auth = (s_state.ext_auth_pages_received & 1u) &&
                               (s_state.ext_auth_pages_received & needed) == needed;
        ESP_LOGD(TAG, "stored Authentication page %u/%u (ready=%d)",
                 page->DataPage, s_state.ext_auth_last_page, s_state.has_ext_auth);
        break;
    }
    case REMOTEID_STORE_UPDATE_TAKEOFF:
        // A new flight: the previous take-off point no longer applies.
        s_state.has_takeoff_position = false;
        if (!s_state.has_position || !location_is_fresh_locked()) {
            ESP_LOGW(TAG, "arm detected but no fresh position, take-off point not captured");
            break;
        }
        s_state.has_takeoff_position   = true;
        s_state.takeoff_latitude       = s_state.latitude;
        s_state.takeoff_longitude      = s_state.longitude;
        s_state.takeoff_altitude_geo_m = s_state.altitude_geo_m;
        ESP_LOGI(TAG, "take-off point captured: %.6f, %.6f @ %.1f m",
                 s_state.takeoff_latitude, s_state.takeoff_longitude, s_state.takeoff_altitude_geo_m);
        break;
    }
    return true;
}

static void process_update_locked(remoteid_store_update_t *update)
{
    // Static: a full state copy is too large to keep on the store task stack.
    static remoteid_state_t previous;
    bool was_ready = s_basic_id_valid && s_operator_id_valid;

    previous = s_state;
    bool accepted = apply_update_locked(update);
    // Reject anything the broadcast encoder cannot represent. Otherwise a single
    // out-of-range field from an input link would stop every transport.
    if (accepted && remoteid_encode_validate(&s_state) != ESP_OK) {
        ESP_LOGW(TAG, "rejected update type %d: the result cannot be encoded", update->type);
        accepted = false;
    }
    if (!accepted) {
        s_state = previous;
        apply_basic_id_validity_locked();
        s_operator_id_valid = operator_id_is_usable(&s_state);
    }

    expire_live_operator_position_locked();
    update_ready_state_locked();
    if (!was_ready && s_basic_id_valid && s_operator_id_valid) {
        ESP_LOGI(TAG, "Remote ID identity complete, broadcasts may start");
    }
}

static void remoteid_store_task(void *arg)
{
    (void)arg;
    remoteid_store_update_t update;

    while (true) {
        // The timeout bounds how late a live operator position expires when no
        // input arrives at all.
        bool received = xQueueReceive(s_update_queue, &update, pdMS_TO_TICKS(REMOTEID_STORE_TICK_MS)) == pdTRUE;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (received) {
            process_update_locked(&update);
        } else {
            expire_live_operator_position_locked();
        }
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
    // Configured identity goes through the same checks as runtime input; an
    // invalid value leaves the store waiting for a valid one.
    if (!normalize_uas_id(s_state.id_type, s_state.uas_id)) {
        memset(s_state.uas_id, 0, sizeof(s_state.uas_id));
    }
    if (!normalize_operator_id(s_state.operator_id)) {
        // An empty ID or the placeholder is "not set"; anything else failed the check.
        s_operator_id_rejected = s_state.operator_id[0] != '\0' &&
                                 strncmp(s_state.operator_id, "CHANGE_ME_OP_ID", ODID_ID_SIZE) != 0;
        memset(s_state.operator_id, 0, sizeof(s_state.operator_id));
    }
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
    } else if (s_operator_id_rejected) {
        health = REMOTEID_HEALTH_OPERATOR_ID_REJECTED;
    } else if (!s_operator_id_valid) {
        health = REMOTEID_HEALTH_WAITING_OPERATOR_ID;
    } else if (!remoteid_selftest_ok()) {
        health = REMOTEID_HEALTH_TRANSPORTS_NOT_RUNNING;
    } else if (REMOTEID_RUNTIME_LOCATION && !location_is_fresh_locked()) {
        health = REMOTEID_HEALTH_LOCATION_STALE;
    } else if (CONFIG_REMOTEID_COMPLIANCE_STRICT && !location_is_complete_locked()) {
        health = REMOTEID_HEALTH_LOCATION_INCOMPLETE;
    } else {
        health = REMOTEID_HEALTH_OK;
    }
    xSemaphoreGive(s_state_mutex);
    return health;
}

const char *remoteid_store_health_text(remoteid_health_t health)
{
    switch (health) {
    case REMOTEID_HEALTH_OK:                     return "";
    case REMOTEID_HEALTH_SELFTEST_FAILED:        return "Remote ID self-test failed";
    case REMOTEID_HEALTH_INVALID_SERIAL:         return "Invalid UAS serial (CTA-2063-A)";
    case REMOTEID_HEALTH_WAITING_OPERATOR_ID:    return "Operator ID not set";
    case REMOTEID_HEALTH_WAITING_BASIC_ID:       return "UAS ID not set";
    case REMOTEID_HEALTH_TRANSPORTS_NOT_RUNNING: return "Remote ID transmitters not running";
    case REMOTEID_HEALTH_LOCATION_STALE:         return "No fresh aircraft location";
    case REMOTEID_HEALTH_OPERATOR_ID_REJECTED:   return "Operator ID failed consistency check";
    case REMOTEID_HEALTH_LOCATION_INCOMPLETE:    return "No aircraft position, height or time";
    }
    return "Remote ID not ready";
}
