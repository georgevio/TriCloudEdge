/**
 * @file main.c
 * @version 72
 * @brief Main application
 */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_netif.h"

#include "utils/config.h"
#include "storage_manager.h"
#include "image_processor.h"
#include "websocket_server.h"

 // linkage with C++
extern "C" {
#include "utils/wifi.h"
#include "utils/time_sync.h"
#include "s3_uploader.h"

#if MQTT_ENABLED
#include "utils/mqtt.h"
#endif
}

static const char* TAG = "MAIN";

/**
 * @brief Callback function to handle AWS Rekognition results from MQTT.
 *
 * Called by the MQTT when a new result arrives.
 * It relays the message to ALL connected WebSocket clients.
 * @param message The JSON payload received from the MQTT topic.
 */
void on_rekognition_result(const char* message) {
    ESP_LOGI(TAG, "Sending Rekognition result to WebSocket client(s).");
    // To ALL connected clients. In reality, it should choose client
    websocket_server_send_text_all(message);
}

/**
 * @brief Configures the log levels for the application.
 * Set the level for all, and then for each individual
 * component can be set to another level for debugging.
 */
static void configure_system_logging() {
    // Initial status printout
    ESP_LOGI(TAG, "LOG LEVEL: %d (5=V, 4=D, 3=I, 2=W, 1=E, 0=N)", DEFAULT_SYSTEM_LOG_LEVEL);

    esp_log_level_set("*", DEFAULT_SYSTEM_LOG_LEVEL);

    // system-level applications (lower case names)
    esp_log_level_set("wifi", ESP_LOG_WARN); 
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp_image", ESP_LOG_WARN);
    esp_log_level_set("boot", ESP_LOG_WARN); // several messages will print BEFORE REACHING THIS!
    esp_log_level_set("cpu_start", ESP_LOG_WARN);
    esp_log_level_set("intr_alloc", ESP_LOG_WARN);
    esp_log_level_set("memory_layout", ESP_LOG_WARN);
    // Annoying warnings! Test extensively...
    esp_log_level_set("FbsLoader", ESP_LOG_ERROR);
    esp_log_level_set("dl", ESP_LOG_ERROR);
    esp_log_level_set("dl::Model", ESP_LOG_ERROR);
    esp_log_level_set("wifi", ESP_LOG_ERROR);  
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);  

    // User-level apps (Uppercase names)
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("WIFI", ESP_LOG_INFO); 
    esp_log_level_set("TIME_SYNC", ESP_LOG_INFO);
    esp_log_level_set("STORAGE_MANAGER", ESP_LOG_INFO);
    esp_log_level_set("MQTT", ESP_LOG_INFO);
    esp_log_level_set("FACE_DB", ESP_LOG_INFO);
    esp_log_level_set("FACE_ENROLLER", ESP_LOG_INFO);
    esp_log_level_set("IMAGE_PROCESSOR", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCKET_SERVER", ESP_LOG_INFO);
    esp_log_level_set("FACE_RECOGN", ESP_LOG_INFO);
    esp_log_level_set("S3_UPLOADER", ESP_LOG_INFO);

    // Example: set S3_UPLOADER only, to be verbose. EASY TO DEBUG!
    //esp_log_level_set("S3_UPLOADER", ESP_LOG_VERBOSE);
}

extern "C" void app_main(void) {

    // Immediately set log level
    configure_system_logging();

    ESP_LOGI(TAG, "Starting main application...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Storage manager failed! STOP!");
        while (1);
    }

    if (WIFI_ENABLED) {
        wifi_init_sta();
        if (!wifi_is_connected()) {
            ESP_LOGE(TAG, "WiFi failed! Most services will fail!");
        }
    }

    if (WIFI_ENABLED && wifi_is_connected()) {

        char time_buffer[64];
        // After Wi-Fi is up, synchronize time.
        if (time_sync_init(time_buffer, sizeof(time_buffer)) != ESP_OK) {
            ESP_LOGE(TAG, "Time sync failed. S3 might not work!");
        }
        else {
            ESP_LOGI(TAG, "Time synchronized: %s", time_buffer);
        }

#if WEBSOCKET_ENABLED
        if (start_websocket_server() == ESP_OK) {
            esp_netif_ip_info_t ip_info;
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_get_ip_info(netif, &ip_info);
                ESP_LOGI(TAG, "\033[1;36m===== WEBSOCKET UP TO IP ADDRESS =====\033[0m");
                ESP_LOGI(TAG, "\033[1;36m    ws://" IPSTR ":%d/ws \033[0m", IP2STR(&ip_info.ip), WEBSOCKET_PORT);
                ESP_LOGI(TAG, "\033[1;36m======================================\033[0m");
            }
        }
        else {
            ESP_LOGE(TAG, "WebSocket server failed...");
        }
#endif

#if MQTT_ENABLED
        esp_mqtt_client_handle_t client = mqtt_aws_init();
        if (client) {
            if (mqtt_start(client) == ESP_OK) {
                ESP_LOGD(TAG, "AWS IoT MQTT sub started...");
                // Register the callback function to relay MQTT results to WebSocket
                mqtt_register_rekognition_callback(on_rekognition_result);
            }
        }
#endif

        // S3 Startup Test Mode
#if S3_STARTUP_TEST_MODE > 0
        bool s3_is_ready = false;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            esp_err_t s3_test_result;

#if S3_STARTUP_TEST_MODE == 2
            ESP_LOGI(TAG, "Starting S3 FULL UPLOAD test (attempt %d of 3)...", attempt);
            s3_test_result = s3_uploader_test_upload();
#else // S3_STARTUP_TEST_MODE == 1, i.e., simple test
            ESP_LOGD(TAG, "Starting S3 CONNECTIVITY test (attempt %d of 3)...", attempt);
            s3_test_result = s3_uploader_test_connectivity();
#endif
            if (s3_test_result == ESP_OK) {
                ESP_LOGI(TAG, "S3 Service is ready.");
                s3_is_ready = true;
                break;
            }
            else {
                if (attempt < 3) {
                    ESP_LOGW(TAG, "S3 service test failed. Retry in 20 seconds...");
                    vTaskDelay(pdMS_TO_TICKS(20000));
                }
                else {
                    ESP_LOGE(TAG, "S3 service failed after 3 attempts...");
                }
            }
        }
        if (!s3_is_ready) {
            ESP_LOGE(TAG, "S3 connection failed!");
        }
#endif // S3_STARTUP_TEST_MODE
    } // End of WIFI_ENABLED && wifi_is_connected()

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
