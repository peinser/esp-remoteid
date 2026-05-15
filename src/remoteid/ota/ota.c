#include "ota.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_OTA_ENABLE

#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "indicator.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#define OTA_BUF_SIZE         4096
#define OTA_BODY_MAX_SMALL   1024
#define OTA_HTTP_STACK_SIZE  8192

static const char *TAG = "remoteid_ota";

static void restart_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_restart(uint32_t delay_ms)
{
    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback = restart_timer_cb,
        .name = "ota_restart",
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000ULL);
    }
}

static bool read_small_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0 || req->content_len >= buf_size) {
        return false;
    }

    int received = 0;
    int remaining = (int)req->content_len;

    while (remaining > 0) {
        int n = httpd_req_recv(req, buf + received, remaining);
        if (n <= 0) {
            return false;
        }
        received += n;
        remaining -= n;
    }

    buf[received] = '\0';
    return true;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", desc->version);
    cJSON_AddStringToObject(root, "idf_version", desc->idf_ver);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "next_partition", next ? next->label : "none");
    cJSON_AddBoolToObject(root, "rollback_possible", esp_ota_check_rollback_is_possible());
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialization failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ESP_OK;
}

static esp_err_t handle_update(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Length required");
        return ESP_FAIL;
    }

    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t rc = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = (int)req->content_len;
    int written = 0;

    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int n = httpd_req_recv(req, (char *)buf, to_read);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload timeout");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            }
            return ESP_FAIL;
        }

        rc = esp_ota_write(ota_handle, buf, n);
        if (rc != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(rc));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        written += n;
        remaining -= n;
        ESP_LOGD(TAG, "OTA upload: %d / %d bytes", written, (int)req->content_len);
    }

    free(buf);

    rc = esp_ota_end(ota_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "OTA validation failed: image may be corrupt or mismatched");
        return ESP_FAIL;
    }

    rc = esp_ota_set_boot_partition(ota_partition);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update complete: %d bytes written to '%s'", written, ota_partition->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Update applied, rebooting\"}");
    schedule_restart(500);
    return ESP_OK;
}

static esp_err_t handle_nvs(httpd_req_t *req)
{
    char *body = malloc(OTA_BODY_MAX_SMALL);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    if (!read_small_body(req, body, OTA_BODY_MAX_SMALL)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Body missing, empty, or too large (max 1023 bytes)");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *ns_j    = cJSON_GetObjectItem(root, "namespace");
    const cJSON *key_j   = cJSON_GetObjectItem(root, "key");
    const cJSON *type_j  = cJSON_GetObjectItem(root, "type");
    const cJSON *value_j = cJSON_GetObjectItem(root, "value");

    if (!cJSON_IsString(ns_j) || !cJSON_IsString(key_j) ||
        !cJSON_IsString(type_j) || !cJSON_IsString(value_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Required fields: namespace, key, type, value");
        return ESP_FAIL;
    }

    const char *ns    = ns_j->valuestring;
    const char *key   = key_j->valuestring;
    const char *type  = type_j->valuestring;
    const char *value = value_j->valuestring;

    nvs_handle_t nvs;
    esp_err_t rc = nvs_open(ns, NVS_READWRITE, &nvs);
    if (rc != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s", ns, esp_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }

    if (strcmp(type, "string") == 0) {
        rc = nvs_set_str(nvs, key, value);
    } else if (strcmp(type, "blob") == 0) {
        size_t value_len = strlen(value);
        size_t decoded_len = 0;
        mbedtls_base64_decode(NULL, 0, &decoded_len,
                              (const unsigned char *)value, value_len);

        uint8_t *decoded = malloc(decoded_len);
        if (!decoded) {
            nvs_close(nvs);
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_FAIL;
        }

        int b64_rc = mbedtls_base64_decode(decoded, decoded_len, &decoded_len,
                                           (const unsigned char *)value, value_len);
        if (b64_rc != 0) {
            free(decoded);
            nvs_close(nvs);
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid base64 value");
            return ESP_FAIL;
        }

        rc = nvs_set_blob(nvs, key, decoded, decoded_len);
        free(decoded);
    } else {
        nvs_close(nvs);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "type must be 'string' or 'blob'");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    if (rc == ESP_OK) {
        rc = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "NVS write('%s'/'%s') failed: %s", ns, key, esp_err_to_name(rc));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NVS updated: namespace='%s' key='%s' type='%s'", ns, key, type);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    char body[128];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    bool confirmed = cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "FACTORY-RESET") == 0;
    cJSON_Delete(root);

    if (!confirmed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Send {\"confirm\":\"FACTORY-RESET\"} to proceed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "factory reset confirmed, erasing NVS partition");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"NVS erased, rebooting\"}");

    nvs_flash_erase();
    schedule_restart(500);
    return ESP_OK;
}

static esp_err_t handle_rollback(httpd_req_t *req)
{
    char body[128];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *confirm = cJSON_GetObjectItem(root, "confirm");
    bool confirmed = cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "ROLLBACK") == 0;
    cJSON_Delete(root);

    if (!confirmed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Send {\"confirm\":\"ROLLBACK\"} to proceed");
        return ESP_FAIL;
    }

    if (!esp_ota_check_rollback_is_possible()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\","
                                "\"message\":\"No previous firmware to roll back to\"}");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "OTA rollback confirmed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Rolling back, rebooting\"}");

    esp_ota_mark_app_invalid_rollback_and_reboot();
    return ESP_OK; // unreachable
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = CONFIG_REMOTEID_OTA_HTTP_PORT;
    config.stack_size      = OTA_HTTP_STACK_SIZE;
    config.lru_purge_enable = true;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "start HTTP server");

    static const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = handle_status
    };
    static const httpd_uri_t uri_update = {
        .uri = "/update", .method = HTTP_POST, .handler = handle_update
    };
    static const httpd_uri_t uri_nvs = {
        .uri = "/nvs", .method = HTTP_POST, .handler = handle_nvs
    };
    static const httpd_uri_t uri_factory_reset = {
        .uri = "/factory-reset", .method = HTTP_POST, .handler = handle_factory_reset
    };
    static const httpd_uri_t uri_rollback = {
        .uri = "/rollback", .method = HTTP_POST, .handler = handle_rollback
    };

    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_update);
    httpd_register_uri_handler(server, &uri_nvs);
    httpd_register_uri_handler(server, &uri_factory_reset);
    httpd_register_uri_handler(server, &uri_rollback);

    return ESP_OK;
}

static esp_err_t start_ota_ap(void)
{
    esp_err_t rc;

    rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return rc;
    }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return rc;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set Wi-Fi AP mode");

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, CONFIG_REMOTEID_OTA_AP_SSID,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len     = strlen(CONFIG_REMOTEID_OTA_AP_SSID);
    ap_config.ap.channel      = CONFIG_REMOTEID_OTA_AP_CHANNEL;
    ap_config.ap.max_connection = 4;

    size_t pass_len = strlen(CONFIG_REMOTEID_OTA_AP_PASSWORD);
    if (pass_len >= 8) {
        strlcpy((char *)ap_config.ap.password, CONFIG_REMOTEID_OTA_AP_PASSWORD,
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Locally-administered random MAC to avoid exposing device identity in OTA mode.
    uint8_t mac[6];
    esp_fill_random(mac, sizeof(mac));
    mac[0] = (mac[0] & 0xfe) | 0x02;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mac(WIFI_IF_AP, mac), TAG, "randomize OTA AP MAC");

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "configure OTA AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start OTA AP");

    return ESP_OK;
}

static bool ota_triggered(void)
{
#if CONFIG_REMOTEID_OTA_ALWAYS_ENTER
    return true;
#elif CONFIG_REMOTEID_OTA_TRIGGER_GPIO >= 0
    gpio_config_t io_conf = {
        .pin_bit_mask  = 1ULL << CONFIG_REMOTEID_OTA_TRIGGER_GPIO,
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(10)); // settle pull-up
    return gpio_get_level(CONFIG_REMOTEID_OTA_TRIGGER_GPIO) == 0;
#else
    return false;
#endif
}

esp_err_t remoteid_ota_check_and_run(void)
{
    if (!ota_triggered()) {
        return ESP_OK;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "OTA mode triggered (firmware %s), starting management server",
             desc->version);

    indicator_set_ota_active();
    ESP_RETURN_ON_ERROR(start_ota_ap(), TAG, "start OTA Wi-Fi AP");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start OTA HTTP server");

    ESP_LOGI(TAG,
             "OTA server ready on http://192.168.4.1:%d, "
             "connect to SSID '%s'%s",
             CONFIG_REMOTEID_OTA_HTTP_PORT,
             CONFIG_REMOTEID_OTA_AP_SSID,
             strlen(CONFIG_REMOTEID_OTA_AP_PASSWORD) >= 8 ? " (WPA2)" : " (open)");
    ESP_LOGI(TAG, "Endpoints: GET /status  POST /update  POST /nvs  "
                  "POST /factory-reset  POST /rollback");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_OK; // Unreachable, infinite loop above.
}

#else // CONFIG_REMOTEID_OTA_ENABLE

esp_err_t remoteid_ota_check_and_run(void)
{
    return ESP_OK;
}

#endif // CONFIG_REMOTEID_OTA_ENABLE
