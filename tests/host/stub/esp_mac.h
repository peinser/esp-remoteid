#pragma once
#include <stdint.h>
#include <string.h>
static inline int esp_efuse_mac_get_default(uint8_t *mac) { const uint8_t m[6] = {0x24, 0x6f, 0x28, 0x01, 0x02, 0x03}; memcpy(mac, m, 6); return 0; }
