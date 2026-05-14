#pragma once

#include "esp_err.h"

esp_err_t remoteid_indicator_init(void);
void remoteid_indicator_mark_transports_started(void);
void remoteid_indicator_set_error(void);
