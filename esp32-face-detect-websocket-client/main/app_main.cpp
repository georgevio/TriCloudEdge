#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "who_camera.h" 
#include "who_human_face_detection.hpp"

// app-level modules
#include "utils/config.h"
#include "utils/wifi.h"
#include "websocket_client.h"
#include "face_sender.h"
#include "utils/heartbeat.h"

// C-style header for time synchronization
extern "C" {
#include "utils/time_sync.h"
}

static EventGroupHandle_t s_app_event_group;
static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueFaceFrame = NULL;

static const char* TAG = "MAIN";

/**
 * @brief System/App-wide log levels.
 * Lowercase names are system apps
 * Uppercase are userspace apps
 * Can be individualy set to different level
 * for easy debugging.
 */
static void configure_system_logging() {
    esp_log_level_set("*", DEFAULT_SYSTEM_LOG_LEVEL);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("cam_hal", ESP_LOG_ERROR); // annoyning cam_hal: EV-VSYNC-OVF
    esp_log_level_set("human_face_detection", ESP_LOG_WARN);
    esp_log_level_set("who_camera", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    esp_log_level_set("ov2640", ESP_LOG_WARN);
    esp_log_level_set("esp32", ESP_LOG_WARN);
    esp_log_level_set("main_task", ESP_LOG_WARN);

    // User space applications (uppercase names)
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("WIFI", ESP_LOG_INFO);
    esp_log_level_set("CAMERA_CONFIG", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCK_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("FACE_SENDER", ESP_LOG_INFO);
    esp_log_level_set("MSG_HANDLER", ESP_LOG_INFO);
    esp_log_level_set("TIME_SYNC", ESP_LOG_INFO);
}

/**
 * @brief Handle WiFi and IP networking events.
 */
static void app_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_app_event_group, WIFI_CONNECTED_BIT | WEBSOCKET_CONNECTED_BIT);
        websocket_client_stop();
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_app_event_group, WIFI_CONNECTED_BIT);
        
        ESP_LOGD(TAG, "Wi-Fi connected, synchronizing time...");
        char time_buffer[64];
        if (time_sync_init(time_buffer, sizeof(time_buffer)) == ESP_OK) {
            ESP_LOGI(TAG, "Time synced: %s", time_buffer);
        } else {
            ESP_LOGE(TAG, "Time synch failed...Using internat clock");
            // COntinue even without time sync
        }

        websocket_client_start(s_app_event_group);
    }
}

/* Compatibility with C */
extern "C" void app_main() {
    configure_system_logging();
    ESP_LOGI(TAG, "\033[38;2;173;216;230mXXX TriCloudEdge - Client XXX\033[0m");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_app_event_group = xEventGroupCreate();
    xQueueAIFrame = xQueueCreate(FRAME_QUEUE_SIZE, sizeof(camera_fb_t*));
    xQueueFaceFrame = xQueueCreate(FRAME_QUEUE_SIZE, sizeof(face_to_send_t*));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_event_handler, NULL));

    wifi_init_sta();

    // camera registration. paraeters have huge impact on image quality & detection!
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 2, xQueueAIFrame);
    
    // find a face
    register_human_face_detection(xQueueAIFrame, NULL, NULL, xQueueFaceFrame);

    // send the detected face
    face_sender_init(s_app_event_group, xQueueAIFrame, xQueueFaceFrame);

#if HEARTBEAT_ON
    heartbeat_init(s_app_event_group);
#endif

    // application tasks
    xTaskCreate(face_sending_task, "face_sender_task", 8192, NULL, 5, NULL);
#if HEARTBEAT_ON
    ESP_LOGI(TAG, "Heartbeat ON, creating task.");
    xTaskCreate(heartbeat_task, "heartbeat_task", 3072, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "All main_app functions started.");
}