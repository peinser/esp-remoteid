#include "indicator.h"

#include "sdkconfig.h"

#ifndef CONFIG_REMOTEID_INDICATOR_RGB_ENABLE
#define CONFIG_REMOTEID_INDICATOR_RGB_ENABLE 0
#endif

#if CONFIG_REMOTEID_INDICATOR_RGB_ENABLE

#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "store.h"

#define REMOTEID_INDICATOR_RMT_RESOLUTION_HZ 10000000
#define REMOTEID_INDICATOR_TASK_STACK 3072
#define REMOTEID_INDICATOR_TASK_PRIORITY 4
#define REMOTEID_INDICATOR_LED_BYTES 3
#define REMOTEID_INDICATOR_UNUSED __attribute__((unused))

typedef struct remoteid_indicator_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} remoteid_indicator_color_t;

typedef struct remoteid_indicator_step {
    bool on;
    uint16_t duration_ms;
} remoteid_indicator_step_t;

typedef enum remoteid_indicator_mode {
    REMOTEID_INDICATOR_MODE_WAITING,
    REMOTEID_INDICATOR_MODE_OPERATIONAL,
    REMOTEID_INDICATOR_MODE_ERROR,
} remoteid_indicator_mode_t;

static const char *TAG = "remoteid_indicator";

static rmt_channel_handle_t s_rmt_channel;
static rmt_encoder_handle_t s_rmt_encoder;
static volatile bool s_transports_started;
static volatile bool s_error;

static const remoteid_indicator_step_t s_status_waiting[] = {
    { true, 250 },
    { false, 1750 },
};

static const remoteid_indicator_step_t s_status_error[] = {
    { true, 100 },
    { false, 100 },
};

static const remoteid_indicator_step_t s_pattern_beacon_short[] REMOTEID_INDICATOR_UNUSED = {
    { true, 100 },
    { false, 900 },
};

static const remoteid_indicator_step_t s_pattern_beacon_50[] REMOTEID_INDICATOR_UNUSED = {
    { true, 500 },
    { false, 500 },
};

static const remoteid_indicator_step_t s_pattern_strobe_single[] REMOTEID_INDICATOR_UNUSED = {
    { true, 60 },
    { false, 1940 },
};

static const remoteid_indicator_step_t s_pattern_strobe_double[] REMOTEID_INDICATOR_UNUSED = {
    { true, 60 },
    { false, 80 },
    { true, 60 },
    { false, 1800 },
};

static uint8_t scale_channel(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * CONFIG_REMOTEID_INDICATOR_RGB_BRIGHTNESS_PERCENT) / 100U);
}

static esp_err_t set_rgb(remoteid_indicator_color_t color)
{
    uint8_t payload[REMOTEID_INDICATOR_LED_BYTES] = {
        scale_channel(color.green),
        scale_channel(color.red),
        scale_channel(color.blue),
    };
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(s_rmt_channel, s_rmt_encoder, payload, sizeof(payload), &tx_config),
                        TAG, "transmit onboard RGB LED data");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_rmt_channel, 100), TAG, "wait for onboard RGB LED update");
    return ESP_OK;
}

static void set_rgb_logged(remoteid_indicator_color_t color)
{
    esp_err_t rc = set_rgb(color);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "failed to update onboard RGB LED: %s", esp_err_to_name(rc));
    }
}

static const remoteid_indicator_step_t *operational_pattern(size_t *count)
{
#if CONFIG_REMOTEID_INDICATOR_PATTERN_DRONE_BEACON_1HZ_50
    *count = sizeof(s_pattern_beacon_50) / sizeof(s_pattern_beacon_50[0]);
    return s_pattern_beacon_50;
#elif CONFIG_REMOTEID_INDICATOR_PATTERN_STROBE_SINGLE
    *count = sizeof(s_pattern_strobe_single) / sizeof(s_pattern_strobe_single[0]);
    return s_pattern_strobe_single;
#elif CONFIG_REMOTEID_INDICATOR_PATTERN_STROBE_DOUBLE
    *count = sizeof(s_pattern_strobe_double) / sizeof(s_pattern_strobe_double[0]);
    return s_pattern_strobe_double;
#else
    *count = sizeof(s_pattern_beacon_short) / sizeof(s_pattern_beacon_short[0]);
    return s_pattern_beacon_short;
#endif
}

static remoteid_indicator_mode_t current_mode(void)
{
    if (s_error) {
        return REMOTEID_INDICATOR_MODE_ERROR;
    }
    if (s_transports_started && remoteid_store_is_ready()) {
        return REMOTEID_INDICATOR_MODE_OPERATIONAL;
    }
    return REMOTEID_INDICATOR_MODE_WAITING;
}

static const remoteid_indicator_step_t *pattern_for_mode(remoteid_indicator_mode_t mode,
                                                         size_t *count,
                                                         remoteid_indicator_color_t *color)
{
    if (mode == REMOTEID_INDICATOR_MODE_ERROR) {
        *count = sizeof(s_status_error) / sizeof(s_status_error[0]);
        *color = (remoteid_indicator_color_t){ .red = 255, .green = 0, .blue = 0 };
        return s_status_error;
    }

    if (mode == REMOTEID_INDICATOR_MODE_OPERATIONAL) {
        *color = (remoteid_indicator_color_t){ .red = 0, .green = 255, .blue = 0 };
        return operational_pattern(count);
    }

    *count = sizeof(s_status_waiting) / sizeof(s_status_waiting[0]);
    *color = (remoteid_indicator_color_t){ .red = 255, .green = 128, .blue = 0 };
    return s_status_waiting;
}

static void remoteid_indicator_task(void *arg)
{
    (void)arg;
    set_rgb_logged((remoteid_indicator_color_t){ 0 });

    while (true) {
        size_t step_count = 0;
        remoteid_indicator_color_t color = { 0 };
        remoteid_indicator_mode_t mode = current_mode();
        const remoteid_indicator_step_t *pattern = pattern_for_mode(mode, &step_count, &color);

        for (size_t i = 0; i < step_count; i++) {
            set_rgb_logged(pattern[i].on ? color : (remoteid_indicator_color_t){ 0 });
            vTaskDelay(pdMS_TO_TICKS(pattern[i].duration_ms));

            if (current_mode() != mode) {
                break;
            }
        }
    }
}

esp_err_t remoteid_indicator_init(void)
{
    if (CONFIG_REMOTEID_INDICATOR_RGB_GPIO < 0) {
        ESP_LOGW(TAG, "onboard RGB indicator enabled but no GPIO configured; indicator disabled");
        return ESP_OK;
    }

    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = CONFIG_REMOTEID_INDICATOR_RGB_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = REMOTEID_INDICATOR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
        .flags.init_level = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_rmt_channel), TAG,
                        "create onboard RGB LED RMT channel");

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&encoder_config, &s_rmt_encoder), TAG,
                        "create onboard RGB LED RMT encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_rmt_channel), TAG, "enable onboard RGB LED RMT channel");

    if (xTaskCreate(remoteid_indicator_task, "remoteid_indicator", REMOTEID_INDICATOR_TASK_STACK,
                    NULL, REMOTEID_INDICATOR_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create onboard RGB indicator task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "onboard RGB indicator enabled on GPIO %d, brightness %d%%",
             CONFIG_REMOTEID_INDICATOR_RGB_GPIO, CONFIG_REMOTEID_INDICATOR_RGB_BRIGHTNESS_PERCENT);
    return ESP_OK;
}

void remoteid_indicator_mark_transports_started(void)
{
    s_transports_started = true;
}

void remoteid_indicator_set_error(void)
{
    s_error = true;
}

#else

esp_err_t remoteid_indicator_init(void)
{
    return ESP_OK;
}

void remoteid_indicator_mark_transports_started(void)
{
}

void remoteid_indicator_set_error(void)
{
}

#endif
