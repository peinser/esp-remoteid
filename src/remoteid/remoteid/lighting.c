#include "lighting.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "pattern.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "store.h"

#ifndef CONFIG_REMOTEID_LIGHTING_ENABLE
#define CONFIG_REMOTEID_LIGHTING_ENABLE 0
#endif

#define REMOTEID_LIGHTING_OUTPUT_COUNT 5
#define REMOTEID_LIGHTING_TASK_STACK 3072
#define REMOTEID_LIGHTING_TASK_PRIORITY 4
#define REMOTEID_LIGHTING_TICK_MS 20

#if CONFIG_REMOTEID_LIGHTING_ENABLE

#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_ENABLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_ENABLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_GPIO
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_GPIO -1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_ACTIVE_HIGH
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_OPEN_DRAIN
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_OPEN_DRAIN 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_SOLID
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_SOLID 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_BEACON_1HZ_50
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_BEACON_1HZ_50 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_SINGLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_SINGLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_DOUBLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_DOUBLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_TRIPLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_STROBE_TRIPLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_FAST_STROBE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PATTERN_FAST_STROBE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT0_PHASE_MS
#define CONFIG_REMOTEID_LIGHTING_OUTPUT0_PHASE_MS 0
#endif

#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_ENABLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_ENABLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_GPIO
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_GPIO -1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_ACTIVE_HIGH
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_OPEN_DRAIN
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_OPEN_DRAIN 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_SOLID
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_SOLID 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_BEACON_1HZ_50
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_BEACON_1HZ_50 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_SINGLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_SINGLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_DOUBLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_DOUBLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_TRIPLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_STROBE_TRIPLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_FAST_STROBE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PATTERN_FAST_STROBE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT1_PHASE_MS
#define CONFIG_REMOTEID_LIGHTING_OUTPUT1_PHASE_MS 0
#endif

#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_ENABLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_ENABLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_GPIO
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_GPIO -1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_ACTIVE_HIGH
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_OPEN_DRAIN
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_OPEN_DRAIN 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_SOLID
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_SOLID 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_BEACON_1HZ_50
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_BEACON_1HZ_50 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_SINGLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_SINGLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_DOUBLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_DOUBLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_TRIPLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_STROBE_TRIPLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_FAST_STROBE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PATTERN_FAST_STROBE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT2_PHASE_MS
#define CONFIG_REMOTEID_LIGHTING_OUTPUT2_PHASE_MS 0
#endif

#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_ENABLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_ENABLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_GPIO
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_GPIO -1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_ACTIVE_HIGH
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_OPEN_DRAIN
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_OPEN_DRAIN 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_SOLID
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_SOLID 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_BEACON_1HZ_50
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_BEACON_1HZ_50 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_SINGLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_SINGLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_DOUBLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_DOUBLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_TRIPLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_STROBE_TRIPLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_FAST_STROBE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PATTERN_FAST_STROBE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT3_PHASE_MS
#define CONFIG_REMOTEID_LIGHTING_OUTPUT3_PHASE_MS 0
#endif

#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_ENABLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_ENABLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_GPIO
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_GPIO -1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_ACTIVE_HIGH
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_OPEN_DRAIN
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_OPEN_DRAIN 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_SOLID
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_SOLID 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_BEACON_1HZ_50
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_BEACON_1HZ_50 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_SINGLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_SINGLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_DOUBLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_DOUBLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_TRIPLE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_STROBE_TRIPLE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_FAST_STROBE
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PATTERN_FAST_STROBE 0
#endif
#ifndef CONFIG_REMOTEID_LIGHTING_OUTPUT4_PHASE_MS
#define CONFIG_REMOTEID_LIGHTING_OUTPUT4_PHASE_MS 0
#endif

typedef struct remoteid_lighting_output_config {
    bool enabled;
    int gpio;
    bool active_high;
    bool open_drain;
    remoteid_pattern_t pattern;
    uint32_t phase_ms;
} remoteid_lighting_output_config_t;

typedef struct remoteid_lighting_output_state {
    bool configured;
} remoteid_lighting_output_state_t;

static const char *TAG = "remoteid_lighting";

static _Atomic bool s_transports_started;
static int64_t s_start_us;
static remoteid_lighting_output_state_t s_outputs[REMOTEID_LIGHTING_OUTPUT_COUNT];

#define OUTPUT_PATTERN(idx) \
    (CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_SOLID ? REMOTEID_PATTERN_SOLID : \
     CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_BEACON_1HZ_50 ? REMOTEID_PATTERN_BEACON_1HZ_50 : \
     CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_STROBE_SINGLE ? REMOTEID_PATTERN_STROBE_SINGLE : \
     CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_STROBE_DOUBLE ? REMOTEID_PATTERN_STROBE_DOUBLE : \
     CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_STROBE_TRIPLE ? REMOTEID_PATTERN_STROBE_TRIPLE : \
     CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PATTERN_FAST_STROBE ? REMOTEID_PATTERN_FAST_STROBE : \
                                                                 REMOTEID_PATTERN_BEACON_1HZ_SHORT)

#define OUTPUT_CONFIG(idx) \
    { \
        .enabled = CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_ENABLE, \
        .gpio = CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_GPIO, \
        .active_high = CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_ACTIVE_HIGH, \
        .open_drain = CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_OPEN_DRAIN, \
        .pattern = OUTPUT_PATTERN(idx), \
        .phase_ms = CONFIG_REMOTEID_LIGHTING_OUTPUT##idx##_PHASE_MS, \
    }

static const remoteid_lighting_output_config_t s_config[REMOTEID_LIGHTING_OUTPUT_COUNT] = {
    OUTPUT_CONFIG(0),
    OUTPUT_CONFIG(1),
    OUTPUT_CONFIG(2),
    OUTPUT_CONFIG(3),
    OUTPUT_CONFIG(4),
};

static int output_level(const remoteid_lighting_output_config_t *config, bool on)
{
    bool active = config->active_high ? on : !on;
    return active ? 1 : 0;
}

static void set_output(size_t index, bool on)
{
    if (!s_outputs[index].configured) {
        return;
    }

    gpio_set_level(s_config[index].gpio, output_level(&s_config[index], on));
}


static bool output_should_be_on(size_t index, int64_t now_us)
{
    const remoteid_lighting_output_config_t *config = &s_config[index];

    if (!s_outputs[index].configured) {
        return false;
    }

    if (!atomic_load_explicit(&s_transports_started, memory_order_acquire) || !remoteid_store_is_ready()) {
        return false;
    }

    uint32_t elapsed_ms = (uint32_t)((now_us - s_start_us) / 1000LL);
    return remoteid_pattern_is_on(config->pattern, elapsed_ms, config->phase_ms);
}

static void lighting_task(void *arg)
{
    (void)arg;

    while (true) {
        int64_t now_us = esp_timer_get_time();
        for (size_t i = 0; i < REMOTEID_LIGHTING_OUTPUT_COUNT; i++) {
            set_output(i, output_should_be_on(i, now_us));
        }
        vTaskDelay(pdMS_TO_TICKS(REMOTEID_LIGHTING_TICK_MS));
    }
}

static bool gpio_already_used(size_t current_index)
{
    for (size_t i = 0; i < current_index; i++) {
        if (s_outputs[i].configured && s_config[i].gpio == s_config[current_index].gpio) {
            return true;
        }
    }
    return false;
}

static esp_err_t configure_output(size_t index)
{
    const remoteid_lighting_output_config_t *config = &s_config[index];

    if (!config->enabled) {
        return ESP_OK;
    }
    if (config->gpio < 0) {
        ESP_LOGW(TAG, "lighting output %u enabled without GPIO; output disabled", index);
        return ESP_OK;
    }
    if (gpio_already_used(index)) {
        ESP_LOGW(TAG, "lighting output %u duplicates GPIO %d; output disabled", index, config->gpio);
        return ESP_OK;
    }

    gpio_config_t pin_config = {
        .pin_bit_mask = 1ULL << config->gpio,
        .mode = config->open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pin_config), TAG, "configure lighting output GPIO");

    s_outputs[index].configured = true;
    set_output(index, false);
    ESP_LOGI(TAG, "lighting output %u enabled on GPIO %d (pattern=%d phase=%lu ms)",
             index, config->gpio, config->pattern, config->phase_ms);
    return ESP_OK;
}

esp_err_t remoteid_lighting_init(void)
{
    s_start_us = esp_timer_get_time();

    for (size_t i = 0; i < REMOTEID_LIGHTING_OUTPUT_COUNT; i++) {
        ESP_RETURN_ON_ERROR(configure_output(i), TAG, "configure lighting output");
    }

    if (xTaskCreate(lighting_task, "remoteid_lighting", REMOTEID_LIGHTING_TASK_STACK,
                    NULL, REMOTEID_LIGHTING_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create lighting task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void remoteid_lighting_mark_transports_started(void)
{
    atomic_store_explicit(&s_transports_started, true, memory_order_release);
}

#else

esp_err_t remoteid_lighting_init(void)
{
    return ESP_OK;
}

void remoteid_lighting_mark_transports_started(void)
{
}

#endif
