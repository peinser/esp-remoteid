#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
static inline esp_err_t nvs_open(const char *n, int m, nvs_handle_t *h) { (void)n; (void)m; (void)h; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *l) { (void)h; (void)k; (void)o; (void)l; return ESP_ERR_NOT_FOUND; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
