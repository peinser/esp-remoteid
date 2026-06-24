#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "opendroneid.h"
#include "types.h"

esp_err_t remoteid_auth_init(void);

// Locally sign the broadcast message set. Returns the last auth page index that
// was populated (0-based), or -1 when local signing is disabled.
int remoteid_auth_sign_bundle(remoteid_message_bundle_t *bundle);
void remoteid_auth_sign_uas_data(ODID_UAS_Data *uas_data);

// Apply authentication received over the input link (MAVLink/DroneCAN) when
// present, superseding any locally generated signature. These never require the
// signing key and are always available.
//
// remoteid_auth_apply_external_bundle encodes the stored auth pages into the
// bundle and returns the last page index written, or -1 when no external auth
// is available. remoteid_auth_apply_external_uas_data fills the ODID_UAS_Data
// auth pages and returns true when external auth was applied.
int remoteid_auth_apply_external_bundle(remoteid_message_bundle_t *bundle, const remoteid_state_t *state);
bool remoteid_auth_apply_external_uas_data(ODID_UAS_Data *uas_data, const remoteid_state_t *state);
