#pragma once

#include "esp_err.h"
#include "opendroneid.h"
#include "types.h"

// Authentication pages come from exactly one source, chosen in Kconfig: none,
// relayed from MAVLink (CONFIG_REMOTEID_AUTH_RELAY) or signed on this module
// (CONFIG_REMOTEID_AUTH_ED25519). Pages of two sources are never mixed.

esp_err_t remoteid_auth_init(void);

// Fill bundle->auth_pages for the messages currently in bundle->messages.
// Returns the last populated page index (0-based), or -1 when there are none.
int remoteid_auth_fill_bundle(remoteid_message_bundle_t *bundle, const remoteid_state_t *state);

// Fill the Auth pages of a message pack whose other messages are already set.
void remoteid_auth_fill_uas_data(ODID_UAS_Data *uas_data, const remoteid_state_t *state);
