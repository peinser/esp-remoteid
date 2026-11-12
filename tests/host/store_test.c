// Host regression test for the Remote ID state store: identity validation, the
// operator ID consistency check, pilot position / take-off fallback, aircraft
// location freshness and sample order, and readiness. Built twice, with and
// without CONFIG_REMOTEID_COMPLIANCE_STRICT (see the host-test Makefile target).
#include "../../src/remoteid/remoteid/store.c"

#include "model.h"

int64_t g_now_us = 100 * 1000000LL;
EventBits_t g_event_bits;

// Collaborators of the store, outside this test's scope.
bool remoteid_selftest_ok(void) { return true; }
esp_err_t remoteid_encode_validate(const remoteid_state_t *state) { (void)state; return ESP_OK; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void submit(remoteid_store_update_t update) { process_update_locked(&update); }
static void advance_ms(int ms) { g_now_us += (int64_t)ms * 1000; }
static remoteid_health_t health(void) { return remoteid_store_health(); }

// ---- Inputs -------------------------------------------------------------------

static void location_at(float timestamp, double latitude, float height, float speed, float direction)
{
    submit((remoteid_store_update_t){ .type = REMOTEID_STORE_UPDATE_LOCATION, .data.location = {
        .has_position = true, .latitude = latitude, .longitude = 4.4, .altitude_geo_m = 100, .altitude_baro_m = INV_ALT,
        .status = ODID_STATUS_GROUND, .speed_horizontal = speed, .speed_vertical = 0, .direction = direction,
        .height = height, .height_type = ODID_HEIGHT_REF_OVER_TAKEOFF, .timestamp = timestamp } });
}

// A live source: one new sample per call, one second apart in source time.
static float g_source_time = 12.5f;
static void live_sample(void)
{
    g_source_time = fmodf(g_source_time + 1.0f, 3600.0f);
    location_at(g_source_time, 50.9, 5.0f, 0.0f, 0.0f);
}

static void arm(bool armed)
{
    submit((remoteid_store_update_t){ .type = REMOTEID_STORE_UPDATE_ARMED, .data.armed.armed = armed });
}

static void operator_id(const char *id)
{
    remoteid_store_update_t update = { .type = REMOTEID_STORE_UPDATE_OPERATOR_ID };
    strncpy(update.data.operator_id.operator_id, id, ODID_ID_SIZE);
    submit(update);
}

static void system_msg(bool has_position, double latitude, uint32_t timestamp, ODID_operator_location_type_t type,
                       ODID_classification_type_t classification)
{
    submit((remoteid_store_update_t){ .type = REMOTEID_STORE_UPDATE_SYSTEM, .data.system = {
        .has_operator_position = has_position, .operator_latitude = latitude, .operator_longitude = 4.0,
        .operator_altitude_geo_m = 10, .operator_location_type = type, .area_count = 1, .area_ceiling_m = INV_ALT,
        .area_floor_m = INV_ALT, .classification_type = classification, .eu_category = ODID_CATEGORY_EU_SPECIFIC,
        .eu_class = ODID_CLASS_EU_CLASS_2, .timestamp = timestamp } });
}

static void system_update(double latitude, uint32_t timestamp)
{
    submit((remoteid_store_update_t){ .type = REMOTEID_STORE_UPDATE_SYSTEM_UPDATE, .data.system_update = {
        .has_operator_position = true, .operator_latitude = latitude, .operator_longitude = 4.0,
        .operator_altitude_geo_m = 10, .timestamp = timestamp } });
}

// The operator position as broadcast.
static bool pilot(double *latitude, ODID_operator_location_type_t *type)
{
    double longitude;
    float altitude;
    return remoteid_operator_position(&s_state, latitude, &longitude, &altitude, type);
}
static ODID_operator_location_type_t pilot_type(void)
{
    double latitude;
    ODID_operator_location_type_t type = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    return pilot(&latitude, &type) ? type : (ODID_operator_location_type_t)-1;
}
static double pilot_latitude(void)
{
    double latitude = 0;
    ODID_operator_location_type_t type;
    return pilot(&latitude, &type) ? latitude : 0;
}

// Let a live pilot position go stale while the aircraft keeps reporting.
static void let_pilot_expire(void)
{
    for (int i = 0; i < 12; i++) {
        advance_ms(1000);
        live_sample();
    }
}

// ---- Validators ----------------------------------------------------------------

static bool operator_id_result(const char *in, const char *expected)
{
    char id[ODID_ID_SIZE + 1] = {0};
    strncpy(id, in, ODID_ID_SIZE);
    bool ok = normalize_operator_id(id);
    return expected ? ok && strcmp(id, expected) == 0 : !ok;
}

static void test_validators(void)
{
    // EU operator number: the 20-character form is verified and the secret removed.
    CHECK(operator_id_result("FIN87astrdge12k8-xyz", "FIN87astrdge12k8"));
    CHECK(operator_id_result("FIN87ASTRDGE12K8-XYZ", "FIN87astrdge12k8"));   // normalised to lower case
    CHECK(operator_id_result("FIN87astrdge12k8-xyy", NULL));                 // wrong secret
    CHECK(operator_id_result("FIN87astrdge12k9-xyz", NULL));                 // wrong check character
    CHECK(operator_id_result("", NULL));
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    CHECK(operator_id_result("FIN87astrdge12k8", NULL));                     // unverifiable without the secret
    CHECK(operator_id_result("fin87astrdge12k8-xyz", NULL));                 // country code must be upper case
#else
    CHECK(operator_id_result("FIN87astrdge12k8", "FIN87astrdge12k8"));
#endif

    // ANSI/CTA-2063-A serial numbers.
    CHECK(serial_is_valid_cta2063a("1ABC5XYZ12"));
    CHECK(serial_is_valid_cta2063a("MFR1F12345678ABCDEFG"));                // 4 + 1 + 15
    CHECK(!serial_is_valid_cta2063a("1abc5XYZ12"));                         // lower case
    CHECK(!serial_is_valid_cta2063a("1ABC5XYZ1O"));                         // letter O
    CHECK(!serial_is_valid_cta2063a("1ABI5XYZ12"));                         // letter I
    CHECK(!serial_is_valid_cta2063a("1ABC6XYZ12"));                         // declared length mismatch
    CHECK(!serial_is_valid_cta2063a("1ABC0"));

    // UAS ID per ID type: text types normalised, binary types kept byte for byte.
    char uas_id[ODID_ID_SIZE + 1] = {0};
    memcpy(uas_id, "X", 1);
    CHECK(!normalize_uas_id(ODID_IDTYPE_NONE, uas_id));
    memset(uas_id, 0, sizeof(uas_id));
    uas_id[3] = 0x42;
    CHECK(normalize_uas_id(ODID_IDTYPE_UTM_ASSIGNED_UUID, uas_id) && uas_id[3] == 0x42);
    memset(uas_id, 0, sizeof(uas_id));
    CHECK(!normalize_uas_id(ODID_IDTYPE_SPECIFIC_SESSION_ID, uas_id));
}

// ---- Store behaviour -------------------------------------------------------------

static void test_identity(void)
{
    // The placeholder operator ID is "not set", not a failed check.
    CHECK(health() == REMOTEID_HEALTH_WAITING_OPERATOR_ID);
    operator_id("");                                                        // nothing provided yet
    CHECK(health() == REMOTEID_HEALTH_WAITING_OPERATOR_ID);
    operator_id("FIN87astrdge12k8-xyy");                                    // failed check: error until fixed
    CHECK(health() == REMOTEID_HEALTH_OPERATOR_ID_REJECTED);
    CHECK(strcmp(remoteid_store_health_text(health()), "Operator ID failed consistency check") == 0);
    operator_id("FIN87astrdge12k8-xyz");
    CHECK(strcmp(s_state.operator_id, "FIN87astrdge12k8") == 0 && health() != REMOTEID_HEALTH_OPERATOR_ID_REJECTED);
    operator_id("FIN87astrdge12k8-abc");                                    // a bad re-upload keeps the old ID
    CHECK(health() == REMOTEID_HEALTH_OPERATOR_ID_REJECTED && strcmp(s_state.operator_id, "FIN87astrdge12k8") == 0);
    operator_id("FIN87astrdge12k8-xyz");

    remoteid_store_update_t basic = { .type = REMOTEID_STORE_UPDATE_BASIC_ID,
                                      .data.basic_id = { .id_type = ODID_IDTYPE_SERIAL_NUMBER } };
    strcpy(basic.data.basic_id.uas_id, "2XYZ5ABC34");
    submit(basic);
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    CHECK(strcmp(s_state.uas_id, "1ABC5XYZ12") == 0);                       // the eFuse serial cannot be replaced
#else
    CHECK(strcmp(s_state.uas_id, "2XYZ5ABC34") == 0);
#endif
}

static void test_location_freshness(void)
{
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_NO_PILOT_POSITION);                    // arm state unknown, no pilot position
    arm(false);
    CHECK(health() == REMOTEID_HEALTH_OK);                                  // disarmed: captured at arming
    advance_ms(3500);
    CHECK(health() == REMOTEID_HEALTH_LOCATION_STALE);
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_OK);

    // A relay repeating the cached sample (same source time) is not fresh.
    float cached = g_source_time;
    for (int i = 0; i < 4; i++) {
        advance_ms(1000);
        location_at(cached, 50.9, 5.0f, 0.0f, 0.0f);
    }
    CHECK(health() == REMOTEID_HEALTH_LOCATION_STALE);

    // Unknown time: broadcast, but never fresh and never stamped with its arrival.
    live_sample();
    for (int i = 0; i < 4; i++) {
        advance_ms(1000);
        location_at(-1.0f, 51.0, 5.0f, 0.0f, 0.0f);
    }
    CHECK(s_state.latitude == 51.0 && s_state.timestamp < 0.0f);
    CHECK(health() == REMOTEID_HEALTH_LOCATION_STALE);
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_OK);

    // Alternating two old samples is not forward progress, and the older one
    // replaces neither the data nor the order reference.
    live_sample();
    float newest = g_source_time;
    advance_ms(1000);
    location_at(newest - 1.0f, 49.0, 5.0f, 0.0f, 0.0f);
    CHECK(s_state.latitude == 50.9 && s_state.location_sample_timestamp == newest);
    for (int i = 0; i < 11; i++) {
        advance_ms(1000);
        location_at(i % 2 ? newest : newest - 1.0f, 49.0, 5.0f, 0.0f, 0.0f);
    }
    CHECK(health() == REMOTEID_HEALTH_LOCATION_STALE && s_state.latitude == 50.9);

    // A sample further ahead than the elapsed time allows is not in order.
    float watermark_ms_age = (float)(now_ms() - s_state.location_updated_ms) / 1000.0f;
    location_at(fmodf(newest + watermark_ms_age + 60.0f, 3600.0f), 49.0, 5.0f, 0.0f, 0.0f);
    CHECK(s_state.location_sample_timestamp == newest);

    // The source resumes with its true time: accepted.
    g_source_time = fmodf(newest + watermark_ms_age, 3600.0f);
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_OK && s_state.latitude == 50.9);

    // A source silent for 31 minutes resumes with its true time.
    advance_ms(31 * 60 * 1000);
    g_source_time = fmodf(g_source_time + 31 * 60 - 1, 3600.0f);
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_OK && s_state.location_sample_timestamp == g_source_time);

    // Across the full hour: reach 3599.5 by letting time pass, then 0.5 one second later.
    float to_end_of_hour = fmodf(3599.5f - g_source_time + 3600.0f, 3600.0f);
    advance_ms((int)(to_end_of_hour * 1000.0f));
    location_at(3599.5f, 50.9, 5.0f, 0.0f, 0.0f);
    CHECK(s_state.location_sample_timestamp == 3599.5f);
    advance_ms(1000);
    location_at(0.5f, 50.9, 5.0f, 0.0f, 0.0f);
    CHECK(s_state.location_sample_timestamp == 0.5f && health() == REMOTEID_HEALTH_OK);
    g_source_time = 0.5f;

    // The source clock steps (here back by 10 minutes): its new time is followed
    // after a run of forward samples, so the broadcast does not freeze.
    for (int i = 0; i < REMOTEID_LOCATION_RESYNC_SAMPLES - 1; i++) {
        advance_ms(1000);
        location_at(3000.0f + i, 52.0, 5.0f, 0.0f, 0.0f);
        CHECK(s_state.latitude != 52.0);
    }
    advance_ms(1000);
    location_at(3000.0f + REMOTEID_LOCATION_RESYNC_SAMPLES - 1, 52.0, 5.0f, 0.0f, 0.0f);
    CHECK(s_state.latitude == 52.0 && health() == REMOTEID_HEALTH_OK);
    g_source_time = 3000.0f + REMOTEID_LOCATION_RESYNC_SAMPLES - 1;

    // A short cyclic replay of old samples never forms such a run.
    newest = g_source_time;
    for (int i = 0; i < 12; i++) {
        advance_ms(1000);
        location_at(newest - 3.0f + (float)(i % 3), 53.0, 5.0f, 0.0f, 0.0f);
    }
    CHECK(health() == REMOTEID_HEALTH_LOCATION_STALE && s_state.latitude != 53.0);
    g_source_time = newest + 12.0f;
    live_sample();
    CHECK(health() == REMOTEID_HEALTH_OK);

#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    // Strict completeness: position, height, time, speed, and course while moving.
    g_source_time += 1.0f;
    location_at(g_source_time, 50.9, INV_ALT, 0.0f, 0.0f);
    CHECK(health() == REMOTEID_HEALTH_LOCATION_INCOMPLETE);
    g_source_time += 1.0f;
    location_at(g_source_time, 50.9, 5.0f, 10.0f, INV_DIR);                   // moving, course unknown
    CHECK(health() == REMOTEID_HEALTH_LOCATION_INCOMPLETE);
    g_source_time += 1.0f;
    location_at(g_source_time, 50.9, 5.0f, INV_SPEED_H, 90.0f);               // speed unknown
    CHECK(health() == REMOTEID_HEALTH_LOCATION_INCOMPLETE);
    g_source_time += 1.0f;
    location_at(g_source_time, 50.9, 5.0f, 0.25f, INV_DIR);                   // nearly stationary: fine
    CHECK(health() == REMOTEID_HEALTH_OK);
    advance_ms(1000);
    location_at(-1.0f, 50.9, 5.0f, 0.0f, 0.0f);                               // unknown time: incomplete
    CHECK(health() == REMOTEID_HEALTH_LOCATION_INCOMPLETE);
#endif
    live_sample();
}

static void test_pilot_position(void)
{
    // A zero-filled System (ArduPilot before the ground station) keeps the configured EU values.
    system_msg(false, 0, 0, ODID_OPERATOR_LOCATION_TYPE_TAKEOFF, ODID_CLASSIFICATION_TYPE_UNDECLARED);
    CHECK(s_state.eu_category == ODID_CATEGORY_EU_OPEN && pilot_type() == (ODID_operator_location_type_t)-1);

    // Arming captures the take-off point; a zero-filled System does not erase it.
    arm(true);
    CHECK(pilot_type() == ODID_OPERATOR_LOCATION_TYPE_TAKEOFF && pilot_latitude() == 50.9);
    system_msg(false, 0, 0, ODID_OPERATOR_LOCATION_TYPE_TAKEOFF, ODID_CLASSIFICATION_TYPE_UNDECLARED);
    CHECK(pilot_latitude() == 50.9);

    // A live pilot position takes over; an older System is out of order.
    system_msg(true, 51.0, 1000, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
    CHECK(pilot_type() == ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS && pilot_latitude() == 51.0);
    CHECK(s_state.eu_category == ODID_CATEGORY_EU_SPECIFIC);
    system_msg(true, 49.0, 999, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
    CHECK(pilot_latitude() == 51.0);

    // The relay repeats the cached System: stale, the take-off point is broadcast,
    // the stored source keeps its live role, and the cache cannot bring it back.
    for (int i = 0; i < 12; i++) {
        advance_ms(1000);
        system_msg(true, 51.0, 1000, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
        live_sample();
    }
    CHECK(s_state.operator_position_expired && pilot_type() == ODID_OPERATOR_LOCATION_TYPE_TAKEOFF);
    CHECK(s_state.operator_location_type == ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS);
    system_msg(true, 51.0, 1000, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
    CHECK(pilot_type() == ODID_OPERATOR_LOCATION_TYPE_TAKEOFF);

    // Recovery through SYSTEM_UPDATE only keeps the live role, so a second loss expires again.
    system_update(51.1, 1012);
    CHECK(!s_state.operator_position_expired && pilot_type() == ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS);
    let_pilot_expire();
    CHECK(s_state.operator_position_expired && pilot_type() == ODID_OPERATOR_LOCATION_TYPE_TAKEOFF);

    // With every input stopped, the store's 1 s tick still expires a live position.
    system_update(51.2, 1030);
    CHECK(!s_state.operator_position_expired);
    for (int i = 0; i < 12; i++) {
        advance_ms(1000);
        expire_live_operator_position_locked();
    }
    CHECK(s_state.operator_position_expired);

    // Unknown time (0) never proves a live position fresh, nor revives an expired one.
    system_update(51.3, 1040);
    for (int i = 0; i < 12; i++) {
        advance_ms(1000);
        system_msg(true, 51.3, 0, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
        live_sample();
    }
    CHECK(s_state.operator_position_expired);
    system_msg(true, 51.4, 0, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
    system_update(51.4, 0);
    CHECK(s_state.operator_position_expired);
    // A full System switching to FIXED is an explicit role change, accepted with time 0.
    system_msg(true, 51.7, 0, ODID_OPERATOR_LOCATION_TYPE_FIXED, ODID_CLASSIFICATION_TYPE_EU);
    CHECK(!s_state.operator_position_expired && pilot_type() == ODID_OPERATOR_LOCATION_TYPE_FIXED);
    // A fixed position never expires.
    let_pilot_expire();
    CHECK(pilot_type() == ODID_OPERATOR_LOCATION_TYPE_FIXED);

    // A new arm without a fresh aircraft position discards the previous take-off point.
    arm(false);
    advance_ms(5000);
    arm(true);
    CHECK(!s_state.has_takeoff_position);
    // Armed, the live pilot position lost and no take-off point: reported.
    system_msg(true, 53.0, 2000, ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS, ODID_CLASSIFICATION_TYPE_EU);
    let_pilot_expire();
    CHECK(health() == REMOTEID_HEALTH_NO_PILOT_POSITION);
    // A first arm state of "armed" (module rebooted in flight) captures nothing.
    s_armed = -1;
    arm(true);
    CHECK(!s_state.has_takeoff_position);
}

int main(void)
{
    static remoteid_state_t initial;
    remoteid_model_init(&initial);
#if CONFIG_REMOTEID_COMPLIANCE_STRICT
    CHECK(strcmp(initial.uas_id, "1ABC5XYZ12") == 0 && initial.id_type == ODID_IDTYPE_SERIAL_NUMBER);  // from eFuse
#else
    strcpy(initial.uas_id, "1ABC5XYZ12");
    initial.id_type = ODID_IDTYPE_SERIAL_NUMBER;
#endif
    initial.eu_category = ODID_CATEGORY_EU_OPEN;
    remoteid_store_start(&initial);

    test_validators();
    test_identity();
    test_location_freshness();
    test_pilot_position();

    printf("store_test (strict=%d): %s\n", CONFIG_REMOTEID_COMPLIANCE_STRICT, failures ? "FAILED" : "passed");
    return failures != 0;
}
