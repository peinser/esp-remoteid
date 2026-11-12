#include "ble.h"

#include <stdint.h>
#include <string.h>

#include "auth.h"
#include "config.h"
#include "encode.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "opendroneid.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"
#include "selftest.h"
#include "store.h"

#define REMOTEID_SERVICE_DATA_LEN    (2 + ODID_MESSAGE_SIZE)
#define REMOTEID_LEGACY_ADV_DATA_LEN (1 + 1 + 2 + REMOTEID_SERVICE_DATA_LEN)
#define REMOTEID_ADV_INTERVAL_100_MS 0x00a0
// One full advertising window per auth page: enough for one transmission on all 3 channels.
#define REMOTEID_BLE_AUTH_PAGE_INTERVAL_MS 100
// HCI Advertising_TX_Power 127: the host has no preference, so the controller
// uses the level set with esp_ble_tx_power_set(). A zero-initialised field
// would instead request 0 dBm for the advertising set.
#define REMOTEID_BLE_TX_POWER_NO_PREFERENCE 127

#ifndef CONFIG_REMOTEID_BLE_TX_INTERVAL_MS
#define CONFIG_REMOTEID_BLE_TX_INTERVAL_MS 250
#endif

// Consecutive failed advertisement updates before the transport is reported as
// failed. One failure can be a transient mbuf shortage; a run of them is not.
#define REMOTEID_BLE_TX_FAILURE_LIMIT 8

// Both transports use the extended advertising API, one instance each.
#if !CONFIG_BT_NIMBLE_EXT_ADV
#error "Remote ID BLE needs CONFIG_BT_NIMBLE_EXT_ADV=y (see sdkconfig.defaults)"
#endif
#if CONFIG_REMOTEID_TRANSPORT_BLE && CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE && \
    CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES < 2
#error "BLE4 legacy + BLE5 Long Range need CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES >= 2"
#endif

static const char *TAG = "remoteid_ble";

// Auth slots exist when an authentication source is configured. Slots beyond
// the populated pages are skipped at runtime without consuming time.
#if CONFIG_REMOTEID_AUTH_ED25519 || CONFIG_REMOTEID_AUTH_RELAY
#define REMOTEID_BLE_AUTH_PAGES 1
#else
#define REMOTEID_BLE_AUTH_PAGES 0
#endif

// The schedule has one slot per auth page.
_Static_assert(ODID_AUTH_MAX_PAGES == 4, "the BLE schedule carries exactly 4 auth page slots");

// Location and System every third slot, the static messages and the auth pages
// (100 ms each) spread between them. With the slot interval T (at most 260 ms):
//   Location / System gap:     at most 3T + 2 x 100 ms = 980 ms   (1 s required)
//   static messages and auth:  every 9T + 4 x 100 ms   = 2740 ms  (3 s required)
static const uint8_t s_message_schedule[] = {
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_BASIC_ID,
#if REMOTEID_BLE_AUTH_PAGES
    REMOTEID_MESSAGE_AUTH_0,
#endif
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_OPERATOR_ID,
#if REMOTEID_BLE_AUTH_PAGES
    REMOTEID_MESSAGE_AUTH_1,
#endif
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_SELF_ID,
#if REMOTEID_BLE_AUTH_PAGES
    REMOTEID_MESSAGE_AUTH_2,
    REMOTEID_MESSAGE_AUTH_3,
#endif
};

#define SCHEDULE_LEN (sizeof(s_message_schedule) / sizeof(s_message_schedule[0]))

// ---- Shared helpers --------------------------------------------------------

static void build_adv_payload(uint8_t out[REMOTEID_LEGACY_ADV_DATA_LEN],
                              const uint8_t message[ODID_MESSAGE_SIZE],
                              uint8_t counter)
{
    out[0] = REMOTEID_LEGACY_ADV_DATA_LEN - 1;
    out[1] = BLE_HS_ADV_TYPE_SVC_DATA_UUID16;
    out[2] = REMOTEID_SERVICE_UUID & 0xff;
    out[3] = (REMOTEID_SERVICE_UUID >> 8) & 0xff;
    out[4] = REMOTEID_AD_APPLICATION_CODE;
    out[5] = counter;
    memcpy(&out[6], message, ODID_MESSAGE_SIZE);
}

static esp_power_level_t configured_ble_tx_power(void)
{
#if CONFIG_REMOTEID_BLE_TX_POWER_N12
    return ESP_PWR_LVL_N12;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N9
    return ESP_PWR_LVL_N9;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N6
    return ESP_PWR_LVL_N6;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N3
    return ESP_PWR_LVL_N3;
#elif CONFIG_REMOTEID_BLE_TX_POWER_N0
    return ESP_PWR_LVL_N0;
#elif CONFIG_REMOTEID_BLE_TX_POWER_P3
    return ESP_PWR_LVL_P3;
#elif CONFIG_REMOTEID_BLE_TX_POWER_P6
    return ESP_PWR_LVL_P6;
#else
    return ESP_PWR_LVL_P9;
#endif
}

static esp_err_t configure_ble_tx_power(void)
{
    esp_power_level_t power = configured_ble_tx_power();
    ESP_RETURN_ON_ERROR(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, power), TAG, "set default BLE TX power");
    ESP_RETURN_ON_ERROR(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, power), TAG, "set advertising BLE TX power");
    ESP_LOGI(TAG, "BLE advertising TX power configured to level %d", power);
    return ESP_OK;
}

// ---- Transport task (shared by BLE4 and BLE5) ------------------------------

#if CONFIG_REMOTEID_TRANSPORT_BLE || CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE

typedef struct {
    uint8_t                   instance;
    const char               *label;
    remoteid_message_bundle_t bundle;
    uint8_t                   counters[ODID_MSG_COUNTER_AMOUNT];
    size_t                    schedule_index;
    bool                      started;
    // Last auth page index of the set being broadcast (0-based), or -1 if none.
    // Set at the page 0 slot; pages 1..N of the same set follow later in the cycle.
    int                       auth_last_page;
    uint8_t                   tx_failures;
    remoteid_transport_t      transport;
} ble_transport_ctx_t;

// Count consecutive failures; a run of them latches a Remote ID system failure.
static void record_tx_result(ble_transport_ctx_t *ctx, bool ok)
{
    if (ok) {
        ctx->tx_failures = 0;
        remoteid_selftest_transmitted(ctx->transport);
        return;
    }
    if (++ctx->tx_failures == REMOTEID_BLE_TX_FAILURE_LIMIT) {
        remoteid_selftest_report(ctx->label, false);
    }
}

static esp_err_t advertise_ble(ble_transport_ctx_t *ctx, const uint8_t message[ODID_MESSAGE_SIZE])
{
    uint8_t adv_data[REMOTEID_LEGACY_ADV_DATA_LEN] = {0};
    uint8_t message_type  = message[0] >> 4;
    uint8_t counter_index = message_type < ODID_MSG_COUNTER_AMOUNT ? message_type : ODID_MSG_COUNTER_PACKED;

    build_adv_payload(adv_data, message, ctx->counters[counter_index]);

    struct os_mbuf *om = os_msys_get_pkthdr(sizeof(adv_data), 0);
    if (!om) {
        ESP_LOGE(TAG, "%s failed to allocate mbuf", ctx->label);
        return ESP_ERR_NO_MEM;
    }
    if (os_mbuf_append(om, adv_data, sizeof(adv_data)) != 0) {
        os_mbuf_free_chain(om);
        ESP_LOGE(TAG, "%s failed to append adv data to mbuf", ctx->label);
        return ESP_FAIL;
    }

    // NimBLE takes ownership of om regardless of return value from here on.
    int rc = ble_gap_ext_adv_set_data(ctx->instance, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s failed to set ext adv data: %d", ctx->label, rc);
        return ESP_FAIL;
    }

    if (!ctx->started) {
        rc = ble_gap_ext_adv_start(ctx->instance, 0, 0);
        if (rc != 0) {
            ESP_LOGE(TAG, "%s failed to start ext adv: %d", ctx->label, rc);
            return ESP_FAIL;
        }
        ctx->started = true;
    }

    ctx->counters[counter_index]++;
    return ESP_OK;
}

static void remoteid_ble_transport_task(void *arg)
{
    ble_transport_ctx_t *ctx = arg;
    remoteid_state_t snapshot;

    ESP_LOGI(TAG, "%s transport waiting for Remote ID identity", ctx->label);
    ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
    ESP_LOGI(TAG, "%s transport ready, starting advertisements", ctx->label);

    // Fixed cadence: the slot period does not stretch by the encode/sign time.
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        if (!remoteid_store_is_ready()) {
            ble_gap_ext_adv_stop(ctx->instance);
            ctx->started = false;
            remoteid_selftest_paused(ctx->transport);
            ESP_LOGW(TAG, "%s transport paused until Remote ID identity is ready", ctx->label);
            ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
            last_wake = xTaskGetTickCount();
        }

        uint8_t schedule_entry = s_message_schedule[ctx->schedule_index];
        ctx->schedule_index = (ctx->schedule_index + 1) % SCHEDULE_LEN;
        bool is_auth_page = (schedule_entry >= REMOTEID_MESSAGE_COUNT);
        int page = schedule_entry - REMOTEID_MESSAGE_COUNT;

        // Pages beyond the populated ones take no advertising interval.
        if (is_auth_page && page > 0 && page > ctx->auth_last_page) {
            continue;
        }

        ESP_ERROR_CHECK(remoteid_store_get_snapshot(&snapshot));
        const uint8_t *message = NULL;

        if (remoteid_encode_static_messages(&snapshot, &ctx->bundle) != ESP_OK) {
            ESP_LOGE(TAG, "%s failed to encode static messages", ctx->label);
        } else if (is_auth_page) {
            if (page == 0) {
                // A local signature covers the bytes last broadcast for each
                // message type; re-encoding Location/System here would diverge
                // (their timestamps advance) and fail verification.
                ctx->auth_last_page = remoteid_auth_fill_bundle(&ctx->bundle, &snapshot);
            }
            if (page > ctx->auth_last_page) {
                continue; // no signature available this cycle
            }
            message = ctx->bundle.auth_pages[page];
        } else {
            remoteid_message_index_t idx = (remoteid_message_index_t)schedule_entry;
            // A failed encode is not hidden behind the previous, now stale, payload.
            if (remoteid_encode_dynamic_message(&snapshot, &ctx->bundle, idx) == ESP_OK) {
                message = ctx->bundle.messages[idx];
            } else {
                ESP_LOGE(TAG, "%s failed to encode message %u", ctx->label, idx);
            }
        }

        bool ok = message && advertise_ble(ctx, message) == ESP_OK;
        record_tx_result(ctx, ok);
        if (ok) {
            ESP_LOGD(TAG, "%s advertising ODID message type %u", ctx->label, message[0] >> 4);
        }

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(is_auth_page ? REMOTEID_BLE_AUTH_PAGE_INTERVAL_MS
                                                               : CONFIG_REMOTEID_BLE_TX_INTERVAL_MS));
    }
}

#endif /* CONFIG_REMOTEID_TRANSPORT_BLE || CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE */

// ---- BLE 4 legacy advertising ----------------------------------------------

#if CONFIG_REMOTEID_TRANSPORT_BLE

// The legacy advertising API (ble_gap_adv_*) is disabled when BLE_EXT_ADV is
// enabled in NimBLE. BLE4 uses the extended advertising API with legacy_pdu=1
// to emit standard BLE4 packets on the 1M PHY. Instance 0 is always BLE4;
// when BLE5 is also active it takes instance 1.
#define BLE4_LEGACY_ADV_INSTANCE 0

static ble_transport_ctx_t s_ble4_ctx = { .instance = BLE4_LEGACY_ADV_INSTANCE, .label = "BLE4",
                                          .auth_last_page = -1, .transport = REMOTEID_TRANSPORT_BLE4 };

static esp_err_t configure_ble4_legacy(void)
{
    const struct ble_gap_ext_adv_params params = {
        .primary_phy   = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .legacy_pdu    = 1,
        .connectable   = 0,
        .scannable     = 0,
        .own_addr_type = BLE_OWN_ADDR_RANDOM,
        .itvl_min      = REMOTEID_ADV_INTERVAL_100_MS,
        .itvl_max      = REMOTEID_ADV_INTERVAL_100_MS,
        .channel_map   = BLE_GAP_ADV_DFLT_CHANNEL_MAP,
        .tx_power      = REMOTEID_BLE_TX_POWER_NO_PREFERENCE,
    };

    int8_t selected_tx_power;
    int rc = ble_gap_ext_adv_configure(BLE4_LEGACY_ADV_INSTANCE, &params, &selected_tx_power, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to configure BLE4 legacy ext adv instance: %d", rc);
        return ESP_FAIL;
    }
    s_ble4_ctx.started = false; // a host reset stops advertising; the task restarts it
    ESP_LOGI(TAG, "BLE4 legacy advertising TX power: %d dBm", selected_tx_power);

    static bool task_created;
    if (task_created) {
        return ESP_OK;
    }
    task_created = true;
    if (xTaskCreate(remoteid_ble_transport_task, "remoteid_ble4", 6144, &s_ble4_ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create BLE4 legacy task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE4 legacy advertising enabled");
    return ESP_OK;
}

#endif /* CONFIG_REMOTEID_TRANSPORT_BLE */

// ---- BLE 5 Long Range (LE Coded PHY) ---------------------------------------

#if CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE

#if CONFIG_REMOTEID_TRANSPORT_BLE
#define BLE5_LR_ADV_INSTANCE 1
#else
#define BLE5_LR_ADV_INSTANCE 0
#endif

static ble_transport_ctx_t s_ble5_ctx = { .instance = BLE5_LR_ADV_INSTANCE, .label = "BLE5",
                                          .auth_last_page = -1, .transport = REMOTEID_TRANSPORT_BLE5 };

static esp_err_t configure_ble5_lr(void)
{
    const struct ble_gap_ext_adv_params params = {
        .primary_phy   = BLE_HCI_LE_PHY_CODED,
        .secondary_phy = BLE_HCI_LE_PHY_CODED,
        .legacy_pdu    = 0,
        .connectable   = 0,
        .scannable     = 0,
        .directed      = 0,
        .own_addr_type = BLE_OWN_ADDR_RANDOM,
        .itvl_min      = REMOTEID_ADV_INTERVAL_100_MS,
        .itvl_max      = REMOTEID_ADV_INTERVAL_100_MS,
        .channel_map   = BLE_GAP_ADV_DFLT_CHANNEL_MAP,
        .tx_power      = REMOTEID_BLE_TX_POWER_NO_PREFERENCE,
    };

    int8_t selected_tx_power;
    int rc = ble_gap_ext_adv_configure(BLE5_LR_ADV_INSTANCE, &params, &selected_tx_power, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to configure BLE5 Long Range ext adv instance: %d", rc);
        return ESP_FAIL;
    }
    s_ble5_ctx.started = false; // a host reset stops advertising; the task restarts it
    ESP_LOGI(TAG, "BLE5 Long Range advertising TX power: %d dBm", selected_tx_power);

    static bool task_created;
    if (task_created) {
        return ESP_OK;
    }
    task_created = true;
    if (xTaskCreate(remoteid_ble_transport_task, "remoteid_ble5", 6144, &s_ble5_ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create BLE5 Long Range task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE5 Long Range (LE Coded PHY) advertising enabled");
    return ESP_OK;
}

#endif /* CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE */

// ---- NimBLE host -----------------------------------------------------------

// NimBLE calls this at start-up and again after every host reset. The random
// address is generated once so receivers keep associating the advertisements
// with the same aircraft for the whole flight.
static void ble_on_sync(void)
{
    static ble_addr_t rnd_addr;
    static bool have_addr;

    if (!have_addr) {
        int rc = ble_hs_id_gen_rnd(1, &rnd_addr);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to generate random BLE address: %d", rc);
            remoteid_selftest_report("BLE", false);
            return;
        }
        have_addr = true;
    }
    int rc = ble_hs_id_set_rnd(rnd_addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set random BLE address: %d", rc);
        remoteid_selftest_report("BLE", false);
        return;
    }

    ESP_LOGI(TAG, "BLE host ready");

#if CONFIG_REMOTEID_TRANSPORT_BLE
    if (configure_ble4_legacy() != ESP_OK) {
        remoteid_selftest_report("BLE4", false);
    }
#endif

#if CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE
    if (configure_ble5_lr() != ESP_OK) {
        remoteid_selftest_report("BLE5", false);
    }
#endif
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason %d), advertising resumes on resync", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t remoteid_ble_start(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "initialize NimBLE");
    ESP_RETURN_ON_ERROR(configure_ble_tx_power(), TAG, "configure BLE TX power");

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    nimble_port_freertos_init(nimble_host_task);

    return ESP_OK;
}
