#pragma once

#include "esp_err.h"

esp_err_t indicator_init(void);
void indicator_mark_transports_started(void);
void indicator_set_ota_active(void);
void indicator_set_error(void);
