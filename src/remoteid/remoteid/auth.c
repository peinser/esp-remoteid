#include "auth.h"

#include "sdkconfig.h"

#if CONFIG_REMOTEID_AUTH_ED25519

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "monocypher-ed25519.h"
#include "nvs.h"

static const char *TAG = "remoteid_auth";

// Fixed 16-byte header for PKCS#8 Ed25519 DER (RFC 8410).
// Followed immediately by the 32-byte private key seed.
static const uint8_t PKCS8_ED25519_PREFIX[16] = {
    0x30, 0x2e,                          // SEQUENCE (46 bytes)
    0x02, 0x01, 0x00,                    // INTEGER version = 0
    0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, // SEQUENCE { OID 1.3.101.112 }
    0x04, 0x22, 0x04, 0x20,              // OCTET STRING { OCTET STRING (32 bytes) }
};

static uint8_t s_private_key[64];

// Advance past a real newline (0x0a) or the two-character escape sequence \n.
// Returns pointer to the first character after the newline, or NULL.
static const char *skip_past_newline(const char *s)
{
    while (*s) {
        if (*s == '\n') return s + 1;
        if (*s == '\\' && *(s + 1) == 'n') return s + 2;
        s++;
    }
    return NULL;
}

static esp_err_t parse_pkcs8_ed25519(const char *pem, uint8_t seed[32])
{
    const char *b64_start = pem;
    const char *b64_end   = pem + strlen(pem);

    if (strncmp(pem, "-----BEGIN", 10) == 0) {
        b64_start = skip_past_newline(pem);
        if (!b64_start) {
            ESP_LOGE(TAG, "malformed PEM: no newline after header");
            return ESP_ERR_INVALID_ARG;
        }
        const char *footer = strstr(b64_start, "-----END");
        if (!footer) {
            ESP_LOGE(TAG, "malformed PEM: missing footer");
            return ESP_ERR_INVALID_ARG;
        }
        b64_end = footer;
    }

    // Strip whitespace and literal \n escape sequences from the base64 body.
    char b64[80];
    size_t b64_len = 0;
    for (const char *p = b64_start; p < b64_end && b64_len < sizeof(b64) - 1; p++) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') continue;
        if (*p == '\\' && p + 1 < b64_end && *(p + 1) == 'n') { p++; continue; }
        b64[b64_len++] = *p;
    }
    b64[b64_len] = '\0';

    uint8_t der[64];
    size_t  der_len = 0;
    if (mbedtls_base64_decode(der, sizeof(der), &der_len,
                              (const uint8_t *)b64, b64_len) != 0) {
        ESP_LOGE(TAG, "private key base64 decode failed");
        return ESP_ERR_INVALID_ARG;
    }

    if (der_len != 48 ||
        memcmp(der, PKCS8_ED25519_PREFIX, sizeof(PKCS8_ED25519_PREFIX)) != 0) {
        memset(der, 0, sizeof(der));
        ESP_LOGE(TAG, "private key is not a PKCS#8 Ed25519 key; use "
                      "'openssl genpkey -algorithm ed25519'");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(seed, der + sizeof(PKCS8_ED25519_PREFIX), 32);
    memset(der, 0, sizeof(der));
    return ESP_OK;
}

static void fill_auth_pages(ODID_Auth_data pages[ODID_AUTH_MAX_PAGES],
                            const uint8_t signature[64])
{
    uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    for (int page = 0; page < ODID_AUTH_MAX_PAGES; page++) {
        ODID_Auth_data *auth = &pages[page];
        odid_initAuthData(auth);
        auth->AuthType = ODID_AUTH_MESSAGE_SET_SIGNATURE;
        auth->DataPage = page;

        if (page == 0) {
            auth->LastPageIndex = ODID_AUTH_MAX_PAGES - 1;
            auth->Length        = 64;
            auth->Timestamp     = timestamp;
            memcpy(auth->AuthData, signature, ODID_AUTH_PAGE_ZERO_DATA_SIZE);
        } else {
            int offset   = ODID_AUTH_PAGE_ZERO_DATA_SIZE +
                           (page - 1) * ODID_AUTH_PAGE_NONZERO_DATA_SIZE;
            int remaining = 64 - offset;
            int copy_len  = remaining < ODID_AUTH_PAGE_NONZERO_DATA_SIZE
                            ? remaining : ODID_AUTH_PAGE_NONZERO_DATA_SIZE;
            if (copy_len > 0) {
                memcpy(auth->AuthData, signature + offset, copy_len);
            }
        }
    }
}

// NVS namespace and key name for the provisioned private key.
#define REMOTEID_AUTH_NVS_NAMESPACE "remoteid_auth"
#define REMOTEID_AUTH_NVS_KEY       "private_key"

// Maximum PEM size: ~120 bytes in practice; 256 gives comfortable headroom.
#define REMOTEID_AUTH_PEM_BUF_SIZE  256

// Load the private key PEM from compiled-in Kconfig value (development) or
// NVS (production). Compiled-in key takes priority so sdkconfig.dev workflow
// is unaffected when both are present.
static esp_err_t load_private_key_pem(char *buf, size_t buf_size)
{
    if (strlen(CONFIG_REMOTEID_AUTH_PRIVATE_KEY_PEM) > 0) {
        strlcpy(buf, CONFIG_REMOTEID_AUTH_PRIVATE_KEY_PEM, buf_size);
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t rc = nvs_open(REMOTEID_AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "no compiled-in key and NVS open failed (%s)", esp_err_to_name(rc));
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = buf_size;
    rc = nvs_get_str(nvs, REMOTEID_AUTH_NVS_KEY, buf, &len);
    nvs_close(nvs);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "private key not in NVS (%s); set REMOTEID_AUTH_PRIVATE_KEY_PEM "
                      "or provision via NVS namespace \"%s\" key \"%s\"",
                 esp_err_to_name(rc), REMOTEID_AUTH_NVS_NAMESPACE, REMOTEID_AUTH_NVS_KEY);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Ed25519 private key loaded from NVS");
    return ESP_OK;
}

esp_err_t remoteid_auth_init(void)
{
    char pem[REMOTEID_AUTH_PEM_BUF_SIZE];
    esp_err_t rc = load_private_key_pem(pem, sizeof(pem));
    if (rc != ESP_OK) {
        return rc;
    }

    uint8_t seed[32];
    rc = parse_pkcs8_ed25519(pem, seed);
    crypto_wipe(pem, sizeof(pem));
    if (rc != ESP_OK) {
        return rc;
    }

    // public_key is a mandatory output of crypto_ed25519_key_pair; NULL is not accepted.
    uint8_t public_key[32];
    crypto_ed25519_key_pair(s_private_key, public_key, seed);
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(public_key, sizeof(public_key));

    return ESP_OK;
}

void remoteid_auth_sign_bundle(remoteid_message_bundle_t *bundle)
{
    uint8_t message_set[REMOTEID_MESSAGE_COUNT * ODID_MESSAGE_SIZE];

    for (int i = 0; i < REMOTEID_MESSAGE_COUNT; i++) {
        memcpy(message_set + i * ODID_MESSAGE_SIZE, bundle->messages[i], ODID_MESSAGE_SIZE);
    }

    uint8_t signature[64];
    crypto_ed25519_sign(signature, s_private_key, message_set, sizeof(message_set));

    ODID_Auth_data auth_data[ODID_AUTH_MAX_PAGES];
    fill_auth_pages(auth_data, signature);

    for (int page = 0; page < ODID_AUTH_MAX_PAGES; page++) {
        ODID_Auth_encoded encoded;
        if (encodeAuthMessage(&encoded, &auth_data[page]) != ODID_SUCCESS) {
            ESP_LOGE(TAG, "failed to encode auth page %d", page);
            return;
        }
        memcpy(bundle->auth_pages[page], &encoded, ODID_MESSAGE_SIZE);
    }
}

void remoteid_auth_sign_uas_data(ODID_UAS_Data *uas_data)
{
    ODID_BasicID_encoded    basic_id;
    ODID_Location_encoded   location;
    ODID_System_encoded     system;
    ODID_OperatorID_encoded operator_id;

    if (encodeBasicIDMessage(&basic_id,       &uas_data->BasicID[0])   != ODID_SUCCESS ||
        encodeLocationMessage(&location,      &uas_data->Location)     != ODID_SUCCESS ||
        encodeSystemMessage(&system,          &uas_data->System)       != ODID_SUCCESS ||
        encodeOperatorIDMessage(&operator_id, &uas_data->OperatorID)   != ODID_SUCCESS) {
        ESP_LOGE(TAG, "message encoding failed during auth signing, skipping auth pages");
        return;
    }

    uint8_t message_set[REMOTEID_MESSAGE_COUNT * ODID_MESSAGE_SIZE];
    memcpy(message_set + 0 * ODID_MESSAGE_SIZE, &basic_id,    ODID_MESSAGE_SIZE);
    memcpy(message_set + 1 * ODID_MESSAGE_SIZE, &location,    ODID_MESSAGE_SIZE);
    memcpy(message_set + 2 * ODID_MESSAGE_SIZE, &system,      ODID_MESSAGE_SIZE);
    memcpy(message_set + 3 * ODID_MESSAGE_SIZE, &operator_id, ODID_MESSAGE_SIZE);

    uint8_t signature[64];
    crypto_ed25519_sign(signature, s_private_key, message_set, sizeof(message_set));

    fill_auth_pages(uas_data->Auth, signature);

    for (int page = 0; page < ODID_AUTH_MAX_PAGES; page++) {
        uas_data->AuthValid[page] = 1;
    }
}

#else // !CONFIG_REMOTEID_AUTH_ED25519

esp_err_t remoteid_auth_init(void) { return ESP_OK; }
void remoteid_auth_sign_bundle(remoteid_message_bundle_t *bundle) { (void)bundle; }
void remoteid_auth_sign_uas_data(ODID_UAS_Data *uas_data) { (void)uas_data; }

#endif
