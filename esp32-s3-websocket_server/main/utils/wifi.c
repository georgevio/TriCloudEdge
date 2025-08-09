#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "wifi.h"
#include "config.h"

static const char* TAG = "WIFI";

// Event group to signal network status
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

// Static flags to hold the current status
static bool s_wifi_connected_status = false;
static bool s_dns_working = false;
static int s_retry_num = 0;

static bool test_dns_resolution(void) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo* res;
    ESP_LOGD(TAG, "Testing DNS resolution...");
    int err = getaddrinfo("example.com", NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS failed: %d", err);
        return false;
    }
    ESP_LOGD(TAG, "DNS resolution OK.");
    freeaddrinfo(res);
    return true;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGD(TAG, "WiFi disconnected, retrying to connect... (attempt %d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        }
        else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_wifi_connected_status = false;
        s_dns_working = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Current IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected_status = true;
        s_retry_num = 0; // Reset retry counter on successful connection

        s_dns_working = test_dns_resolution();
        if (s_dns_working) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        else {
            ESP_LOGE(TAG, "DNS failed. Some services will fail (e.g., AWS).");
            // DISCUSSION: treat DNS failure as a connection failure?
            // xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGD(TAG, "wifi_init_sta finished. Waiting for network connection...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Network ready: Wi-Fi & DNS ok!");
    }
    else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts. Check credentials in secret.h", WIFI_MAXIMUM_RETRY);
    }
    else {
        ESP_LOGE(TAG, "WiFi connection attempt timed out or failed unexpectedly.");
    }
}

bool wifi_is_connected(void)
{
    return s_wifi_connected_status && s_dns_working;
}
