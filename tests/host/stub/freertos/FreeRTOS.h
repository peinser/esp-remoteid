#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#define BIT0 BIT(0)
