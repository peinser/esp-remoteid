#include "led.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifndef CONFIG_REMOTEID_TX_LED_GPIO
#define CONFIG_REMOTEID_TX_LED_GPIO -1
#endif

#ifndef CONFIG_REMOTEID_TX_LED_ACTIVE_HIGH
#define CONFIG_REMOTEID_TX_LED_ACTIVE_HIGH 1
#endif

#ifndef CONFIG_REMOTEID_TX_LED_PULSE_MS
#define CONFIG_REMOTEID_TX_LED_PULSE_MS 40
#endif

static const char *TAG = "remoteid_led";

#if CONFIG_REMOTEID_TX_LED_GPIO >= 0
static esp_timer_handle_t s_led_timer;

static int led_on_level(void)
{
    return CONFIG_REMOTEID_TX_LED_ACTIVE_HIGH ? 1 : 0;
}

static int led_off_level(void)
{
    return CONFIG_REMOTEID_TX_LED_ACTIVE_HIGH ? 0 : 1;
}

static void turn_led_off(void *arg)
{
    (void)arg;
    gpio_set_level(CONFIG_REMOTEID_TX_LED_GPIO, led_off_level());
}

esp_err_t remoteid_led_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_REMOTEID_TX_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure TX status LED GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(CONFIG_REMOTEID_TX_LED_GPIO, led_off_level()), TAG, "turn TX status LED off");

    const esp_timer_create_args_t timer_args = {
        .callback = turn_led_off,
        .name = "remoteid_tx_led",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_led_timer), TAG, "create TX status LED timer");

    ESP_LOGI(TAG, "TX status LED enabled on GPIO %d, active %s, pulse %d ms", CONFIG_REMOTEID_TX_LED_GPIO,
             CONFIG_REMOTEID_TX_LED_ACTIVE_HIGH ? "high" : "low", CONFIG_REMOTEID_TX_LED_PULSE_MS);

    return ESP_OK;
}

void remoteid_led_pulse(void)
{
    gpio_set_level(CONFIG_REMOTEID_TX_LED_GPIO, led_on_level());
    esp_timer_stop(s_led_timer);
    esp_err_t rc = esp_timer_start_once(s_led_timer, CONFIG_REMOTEID_TX_LED_PULSE_MS * 1000ULL);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "failed to start TX status LED pulse timer: %s", esp_err_to_name(rc));
        gpio_set_level(CONFIG_REMOTEID_TX_LED_GPIO, led_off_level());
    }
}
#else
esp_err_t remoteid_led_init(void)
{
    ESP_LOGI(TAG, "TX status LED disabled; set ESP Remote ID -> TX status LED GPIO to pulse an LED");
    return ESP_OK;
}

void remoteid_led_pulse(void)
{
    // TODO Implement
}
#endif
