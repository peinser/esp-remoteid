#pragma once

#include "esp_err.h"

// Enter OTA mode if triggered (GPIO asserted or always-enter Kconfig option).
// Starts a WiFi AP and HTTP management server. Does not return when triggered;
// all server operations schedule a device restart to exit OTA mode.
// Returns ESP_OK immediately when OTA is disabled or the trigger is not asserted.
esp_err_t remoteid_ota_check_and_run(void);
