#pragma once
#include "freertos/FreeRTOS.h"
static inline QueueHandle_t xQueueCreate(int len, int size) { (void)len; (void)size; return (void *)1; }
static inline int xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdTRUE; }
static inline int xQueueReceive(QueueHandle_t q, void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdFALSE; }
