#include "heartbeat.h"
#include "websocket_client.h"
#include "config.h"

static EventGroupHandle_t s_app_event_group;

/**
 * @brief Initialize the heartbeat module with the application event group.
 * @param event_group Handle to the main application event group.
 */
void heartbeat_init(EventGroupHandle_t event_group) {
    s_app_event_group = event_group;
}

/**
 * @brief Periodically send a heartbeat message to the WebSocket.
 * @param pvParameters Unused task parameters.
 * Waits for both WiFi and WebSocket connections to be active, 
 * then sends a keep-alive message at a defined interval 
 * (HEARTBEAT_INTERVAL_S in config.h).
 * Questionable necessity!
 */
void heartbeat_task(void* pvParameters) {
    while(true) {
        xEventGroupWaitBits(s_app_event_group, 
                WIFI_CONNECTED_BIT | WEBSOCKET_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_S * 1000));
        websocket_send_heartbeat();
    }
}