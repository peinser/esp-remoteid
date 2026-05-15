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
#include "store.h"

#define REMOTEID_SERVICE_DATA_LEN    (2 + ODID_MESSAGE_SIZE)
#define REMOTEID_LEGACY_ADV_DATA_LEN (1 + 1 + 2 + REMOTEID_SERVICE_DATA_LEN)
#define REMOTEID_ADV_INTERVAL_100_MS 0x00a0
// One full advertising window between auth pages: enough for one transmission on all 3 channels.
// Auth pages are already consecutive in the schedule so the total burst is 4 x this value.
#define REMOTEID_BLE_AUTH_PAGE_INTERVAL_MS 100

#ifndef CONFIG_REMOTEID_BLE_TX_INTERVAL_MS
#define CONFIG_REMOTEID_BLE_TX_INTERVAL_MS 250
#endif

static const char *TAG = "remoteid_ble";

static const uint8_t s_message_schedule[] = {
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_BASIC_ID,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_SYSTEM,
    REMOTEID_MESSAGE_LOCATION,
    REMOTEID_MESSAGE_OPERATOR_ID,
    REMOTEID_MESSAGE_LOCATION,
#if CONFIG_REMOTEID_AUTH_ED25519
    REMOTEID_MESSAGE_AUTH_0,
    REMOTEID_MESSAGE_AUTH_1,
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
} ble_transport_ctx_t;

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

    while (true) {
        if (!remoteid_store_is_ready()) {
            ble_gap_ext_adv_stop(ctx->instance);
            ctx->started = false;
            ESP_LOGW(TAG, "%s transport paused until Remote ID identity is ready", ctx->label);
            ESP_ERROR_CHECK(remoteid_store_wait_ready(portMAX_DELAY));
        }

        ESP_ERROR_CHECK(remoteid_store_get_snapshot(&snapshot));
        if (remoteid_encode_static_messages(&snapshot, &ctx->bundle) != ESP_OK) {
            ESP_LOGE(TAG, "%s failed to encode static messages", ctx->label);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_REMOTEID_BLE_TX_INTERVAL_MS));
            continue;
        }

        uint8_t schedule_entry = s_message_schedule[ctx->schedule_index];
        bool is_auth_page = (schedule_entry >= REMOTEID_MESSAGE_COUNT);

        if (is_auth_page) {
            int page = schedule_entry - REMOTEID_MESSAGE_COUNT;
            if (page == 0) {
                // Sign the bundle as-is: these are exactly the bytes last broadcast
                // for each message type. Re-encoding here would diverge (system timestamp
                // advances every second) causing receivers to fail verification.
                remoteid_auth_sign_bundle(&ctx->bundle);
            }
            if (advertise_ble(ctx, ctx->bundle.auth_pages[page]) == ESP_OK) {
                ESP_LOGD(TAG, "%s advertising ODID auth page %d", ctx->label, page);
            }
        } else {
            remoteid_message_index_t idx = (remoteid_message_index_t)schedule_entry;
            if (remoteid_encode_dynamic_message(&snapshot, &ctx->bundle, idx) != ESP_OK) {
                ESP_LOGE(TAG, "%s failed to encode dynamic message %u, broadcasting stale", ctx->label, idx);
            }
            if (advertise_ble(ctx, ctx->bundle.messages[idx]) == ESP_OK) {
                ESP_LOGD(TAG, "%s advertising ODID message type %u", ctx->label,
                         ctx->bundle.messages[idx][0] >> 4);
            }
        }

        ctx->schedule_index = (ctx->schedule_index + 1) % SCHEDULE_LEN;
        vTaskDelay(pdMS_TO_TICKS(is_auth_page ? REMOTEID_BLE_AUTH_PAGE_INTERVAL_MS
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

static ble_transport_ctx_t s_ble4_ctx = { .instance = BLE4_LEGACY_ADV_INSTANCE, .label = "BLE4" };

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
    };

    int rc = ble_gap_ext_adv_configure(BLE4_LEGACY_ADV_INSTANCE, &params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to configure BLE4 legacy ext adv instance: %d", rc);
        return ESP_FAIL;
    }

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

static ble_transport_ctx_t s_ble5_ctx = { .instance = BLE5_LR_ADV_INSTANCE, .label = "BLE5" };

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
    };

    int rc = ble_gap_ext_adv_configure(BLE5_LR_ADV_INSTANCE, &params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to configure BLE5 Long Range ext adv instance: %d", rc);
        return ESP_FAIL;
    }

    if (xTaskCreate(remoteid_ble_transport_task, "remoteid_ble5", 6144, &s_ble5_ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create BLE5 Long Range task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BLE5 Long Range (LE Coded PHY) advertising enabled");
    return ESP_OK;
}

#endif /* CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE */

// ---- NimBLE host -----------------------------------------------------------

static void ble_on_sync(void)
{
    ble_addr_t rnd_addr;
    int rc = ble_hs_id_gen_rnd(1, &rnd_addr);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to generate random BLE address: %d", rc);
        return;
    }
    rc = ble_hs_id_set_rnd(rnd_addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set random BLE address: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE host ready");

#if CONFIG_REMOTEID_TRANSPORT_BLE
    if (configure_ble4_legacy() != ESP_OK) {
        ESP_LOGE(TAG, "BLE4 legacy setup failed");
    }
#endif

#if CONFIG_REMOTEID_TRANSPORT_BLE5_LONG_RANGE
    if (configure_ble5_lr() != ESP_OK) {
        ESP_LOGE(TAG, "BLE5 Long Range setup failed");
    }
#endif
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

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(nimble_host_task);

    return ESP_OK;
}
