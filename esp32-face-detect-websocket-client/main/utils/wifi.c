#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "secret.h" // WIFI_SSID & WIFI_PASSWORD. You have to create the file!

static const char* TAG = "WIFI";

// Most of the errors and the lifecycle are in app_main.cpp as the monitoring app!
void wifi_init_sta(void) {
    ESP_LOGI(TAG, "Initializing WiFi client...");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGD(TAG, "WiFi ON.");
}
