#pragma once

#include "esp_err.h"
#include "types.h"

esp_err_t remoteid_encode_static_messages(const remoteid_state_t *state, remoteid_message_bundle_t *bundle);
esp_err_t remoteid_encode_dynamic_message(const remoteid_state_t *state, remoteid_message_bundle_t *bundle,
                                          remoteid_message_index_t message_index);
esp_err_t remoteid_encode_uas_data(const remoteid_state_t *state, ODID_UAS_Data *uas_data);
