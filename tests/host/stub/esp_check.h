#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(c, e, tag, ...) do { (void)(tag); if (!(c)) return (e); } while (0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { (void)(tag); esp_err_t _e = (x); if (_e) return _e; } while (0)
