#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct { struct { uint32_t extd:1, rtr:1; }; uint32_t identifier; uint8_t data_length_code; uint8_t data[8]; } twai_message_t;
typedef struct { int unused; } twai_timing_config_t;
typedef struct { int tx, rx, mode; int rx_queue_len, tx_queue_len; } twai_general_config_t;
typedef struct { int unused; } twai_filter_config_t;
typedef enum { TWAI_STATE_STOPPED, TWAI_STATE_RUNNING, TWAI_STATE_BUS_OFF, TWAI_STATE_RECOVERING } twai_state_t;
typedef struct { twai_state_t state; } twai_status_info_t;
#define TWAI_MODE_NORMAL 0
#define TWAI_TIMING_CONFIG_125KBITS() {0}
#define TWAI_TIMING_CONFIG_250KBITS() {0}
#define TWAI_TIMING_CONFIG_500KBITS() {0}
#define TWAI_TIMING_CONFIG_1MBITS() {0}
#define TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode) {tx, rx, mode, 5, 5}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() {0}
// Defined by the test: records transmitted frames.
esp_err_t twai_transmit(const twai_message_t *message, TickType_t ticks);
static inline esp_err_t twai_receive(twai_message_t *m, TickType_t t) { (void)m; (void)t; return ESP_ERR_TIMEOUT; }
static inline esp_err_t twai_get_status_info(twai_status_info_t *i) { i->state = TWAI_STATE_RUNNING; return ESP_OK; }
static inline esp_err_t twai_initiate_recovery(void) { return ESP_OK; }
static inline esp_err_t twai_start(void) { return ESP_OK; }
static inline esp_err_t twai_driver_install(const twai_general_config_t *g, const twai_timing_config_t *t, const twai_filter_config_t *f)
{ (void)g; (void)t; (void)f; return ESP_OK; }
