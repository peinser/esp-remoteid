#pragma once
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
typedef int esp_efuse_desc_t;
// The provisioned serial in BLOCK_USR_DATA.
static inline esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[], void *dst, size_t bits)
{
    (void)field;
    memset(dst, 0, bits / 8);
    memcpy(dst, "1ABC5XYZ12", 10);
    return ESP_OK;
}
static inline bool esp_efuse_read_field_bit(const esp_efuse_desc_t *field[]) { (void)field; return true; }
