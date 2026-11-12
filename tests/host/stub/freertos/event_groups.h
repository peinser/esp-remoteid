#pragma once
#include "freertos/FreeRTOS.h"
extern EventBits_t g_event_bits;
static inline EventGroupHandle_t xEventGroupCreate(void) { return (void *)1; }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t b) { (void)e; return g_event_bits |= b; }
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t b) { (void)e; return g_event_bits &= ~b; }
static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t e) { (void)e; return g_event_bits; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t b, int c, int a, TickType_t t)
{ (void)e; (void)b; (void)c; (void)a; (void)t; return g_event_bits; }
