// Host regression test for local Ed25519 signing: the signature reassembled
// from the broadcast auth pages verifies over the message set followed by the
// page 0 timestamp, and fails for a changed timestamp (replay under a fresh one).
#define CONFIG_REMOTEID_AUTH_ED25519 1
#include "../../src/remoteid/remoteid/auth.c"

#include <stdlib.h>

static const uint32_t TIMESTAMP = 250000000;
uint32_t remoteid_clock_odid_2019(void) { return TIMESTAMP; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    uint8_t seed[32], public_key[32];
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(i * 7 + 1);
    }
    crypto_ed25519_key_pair(s_private_key, public_key, seed);
    s_key_loaded = true;

    remoteid_message_bundle_t bundle;
    for (int m = 0; m < REMOTEID_MESSAGE_COUNT; m++) {
        for (int b = 0; b < ODID_MESSAGE_SIZE; b++) {
            bundle.messages[m][b] = (uint8_t)rand();
        }
    }
    int last_page = remoteid_auth_fill_bundle(&bundle, NULL);
    CHECK(last_page == 3);

    // Reassemble the signature as a receiver does.
    uint8_t signature[64] = {0};
    uint32_t page0_timestamp = 0;
    for (int page = 0; page <= last_page; page++) {
        ODID_Auth_encoded encoded;
        ODID_Auth_data data;
        memcpy(&encoded, bundle.auth_pages[page], sizeof(encoded));
        CHECK(decodeAuthMessage(&data, &encoded) == ODID_SUCCESS);
        CHECK(data.DataPage == page && data.AuthType == ODID_AUTH_MESSAGE_SET_SIGNATURE);
        if (page == 0) {
            CHECK(data.LastPageIndex == 3 && data.Length == 64);
            page0_timestamp = data.Timestamp;
            memcpy(signature, data.AuthData, ODID_AUTH_PAGE_ZERO_DATA_SIZE);
        } else {
            int offset = ODID_AUTH_PAGE_ZERO_DATA_SIZE + (page - 1) * ODID_AUTH_PAGE_NONZERO_DATA_SIZE;
            int length = 64 - offset < ODID_AUTH_PAGE_NONZERO_DATA_SIZE ? 64 - offset : ODID_AUTH_PAGE_NONZERO_DATA_SIZE;
            memcpy(signature + offset, data.AuthData, length);
        }
    }
    CHECK(page0_timestamp == TIMESTAMP);

    uint8_t signed_data[REMOTEID_MESSAGE_SET_SIZE + 4];
    memcpy(signed_data, bundle.messages, REMOTEID_MESSAGE_SET_SIZE);
    for (int i = 0; i < 4; i++) {
        signed_data[REMOTEID_MESSAGE_SET_SIZE + i] = (uint8_t)(page0_timestamp >> (8 * i));
    }
    CHECK(crypto_ed25519_check(signature, public_key, signed_data, sizeof(signed_data)) == 0);
    signed_data[REMOTEID_MESSAGE_SET_SIZE] ^= 1;                         // the same signature, another timestamp
    CHECK(crypto_ed25519_check(signature, public_key, signed_data, sizeof(signed_data)) != 0);

    // The message pack path signs too.
    ODID_UAS_Data uas_data;
    odid_initUasData(&uas_data);
    strcpy(uas_data.BasicID[0].UASID, "1ABC5XYZ12");
    uas_data.BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    strcpy(uas_data.OperatorID.OperatorId, "FIN87astrdge12k8");
    remoteid_auth_fill_uas_data(&uas_data, NULL);
    CHECK(uas_data.AuthValid[0] && uas_data.AuthValid[3] && uas_data.Auth[0].Timestamp == TIMESTAMP);

    printf("auth_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
