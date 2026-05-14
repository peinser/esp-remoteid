#pragma once

#include "esp_err.h"
#include "opendroneid.h"
#include "types.h"

esp_err_t remoteid_auth_init(void);
void remoteid_auth_sign_bundle(remoteid_message_bundle_t *bundle);
void remoteid_auth_sign_uas_data(ODID_UAS_Data *uas_data);
