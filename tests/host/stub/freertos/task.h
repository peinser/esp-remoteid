#pragma once
#include "freertos/FreeRTOS.h"
// Tasks never run in host tests: the tests call the task bodies' functions directly.
static inline int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; return pdPASS; }
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
