#include "esp_bt.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "remoteid/ble.h"
#include "remoteid/led.h"
#include "remoteid/model.h"
#include "remoteid/wifi.h"
#include "sdkconfig.h"

void app_main(void)
{
    static remoteid_state_t state;

    // TODO DO NOT FORGET When remoteid_state_t is no longer static (e.g., when fed through MAVLlink or other channels)
    // ensure access is thread-safe of check how freertos handles it.

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(remoteid_model_init(&state));
    ESP_ERROR_CHECK(remoteid_led_init());

#if CONFIG_REMOTEID_TRANSPORT_BLE
    ESP_ERROR_CHECK(remoteid_ble_start(&state));
#endif

#if CONFIG_REMOTEID_TRANSPORT_WIFI_BEACON || CONFIG_REMOTEID_TRANSPORT_WIFI_NAN
    ESP_ERROR_CHECK(remoteid_wifi_start(&state));
#endif
}
