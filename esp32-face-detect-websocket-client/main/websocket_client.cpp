#include "websocket_client.h"
#include "message_handler.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "utils/secret.h"
#include "utils/config.h"
#include "freertos/event_groups.h"
#include <string.h>

static esp_websocket_client_handle_t client = NULL;
static EventGroupHandle_t s_client_event_group = NULL;

static const char* TAG = "WEBSOCK_CLIENT";

/**
 * @brief Handles events from the (ESP-IDF) WebSocket client.
 * @param handler_args User arguments.
 * @param base The event base.
 * @param event_id The event ID.
 * @param event_data Data associated with the event.
 * This callback manages connection status and delegates 
 * incoming data to the message_handler.
 */
static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    switch (event_id){
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to : %s", WEBSOCKET_URI);
            if (s_client_event_group) {
                xEventGroupSetBits(s_client_event_group, WEBSOCKET_CONNECTED_BIT);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            if (s_client_event_group) {
                xEventGroupClearBits(s_client_event_group, WEBSOCKET_CONNECTED_BIT | FRAME_ACK_BIT);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "WEBSOCKET_EVENT_DATA received, opcode=%d", data->op_code);
            if (data->op_code == 1 && data->data_ptr) { // Text frame
                char* text_payload = (char*)malloc(data->data_len + 1);
                if (text_payload) {
                    memcpy(text_payload, data->data_ptr, data->data_len);
                    text_payload[data->data_len] = '\0';
                    
                    // Done working: Pass the message data to the message handler function. 
                    message_handler_process(text_payload, s_client_event_group);
                    free(text_payload);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for incoming message.");
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
            if (s_client_event_group) {
                xEventGroupClearBits(s_client_event_group, WEBSOCKET_CONNECTED_BIT | FRAME_ACK_BIT);
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Initialize and start the WebSocket client.
 * @param event_group The application's event group handle for 
 * signaling connection status.
 * Setup client configuration (URI, timeouts), register the event
 * handler, and initiate connection process. 
 * If the client is already running, it will be stopped and restarted.
 */
void websocket_client_start(EventGroupHandle_t event_group) {
    if (client) {
        ESP_LOGW(TAG, "WebSocket client is already active. Restarting...");
        websocket_client_stop();
    }

    s_client_event_group = event_group;

    ESP_LOGD(TAG, "Starting WebSocket client for URI: %s", WEBSOCKET_URI);

    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = WEBSOCKET_URI;
    websocket_cfg.reconnect_timeout_ms = ESP_WEBSOCKET_CLIENT_RETRY_MS;
    websocket_cfg.network_timeout_ms = ESP_WEBSOCKET_CLIENT_SEND_TIMEOUT_MS;
    // Large buffer for potential future uses. Affects device resources!
    websocket_cfg.buffer_size = 160 * 1024; 

    client = esp_websocket_client_init(&websocket_cfg);
    if (!client) {
        ESP_LOGE(TAG, "WebSocket client initialization failed");
        return;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)client);
    esp_websocket_client_start(client);
}

/**
 * @brief Stop and destroy the active WebSocket client instance.
 * Safely disconnects the client, frees all allocated resources,
 * and clears the connection status bits in the event group.
 */
void websocket_client_stop(void) {
    if (client) {
        ESP_LOGD(TAG, "Stopping WebSocket client...");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
        if (s_client_event_group) {
            xEventGroupClearBits(s_client_event_group, WEBSOCKET_CONNECTED_BIT | FRAME_ACK_BIT);
        }
        ESP_LOGD(TAG, "WebSocket client stopped and destroyed.");
    }
}

/**
 * @brief Send a binary data frame over the WebSocket connection.
 * @param data Pointer to the data buffer to send.
 * @param len The length of the data in Bytes.
 * @return ESP_OK on success, ESP_FAIL on failure.
 * Used for sending large binary payloads, e.g., image in chunks.
 * It checks for a valid connection before start sending.
 */
esp_err_t websocket_send_frame(const uint8_t* data, size_t len) {
    if (!client || !is_websocket_connected()) {
        ESP_LOGE(TAG, "Cannot send frame: client not initialized or connected.");
        return ESP_FAIL;
    }
    if (!data || len == 0) {
        ESP_LOGE(TAG, "Invalid data or length for sending frame.");
        return ESP_ERR_INVALID_ARG;
    }
    
    int bytes_sent = esp_websocket_client_send_bin(client, (const char*)data, len, pdMS_TO_TICKS(ESP_WEBSOCKET_CLIENT_SEND_TIMEOUT_MS));
    if (bytes_sent < 0) {
        ESP_LOGE(TAG, "Error sending binary frame via WebSocket");
        return ESP_FAIL;
    }
    if ((size_t)bytes_sent < len) {
        ESP_LOGW(TAG, "Incomplete frame chunk sent: %d of %zu bytes.", bytes_sent, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Send a text message over the WebSocket connection.
 * @param text A null-terminated string to send.
 * @return ESP_OK on success, ESP_FAIL on failure.
 * Used for sending control messages as JSON strings.
 */
esp_err_t websocket_send_text(const char* text) {
    if (!client || !is_websocket_connected()) {
        ESP_LOGE(TAG, "Cannot send text: client not initialized or connected.");
        return ESP_FAIL;
    }
     if (!text) {
        ESP_LOGE(TAG, "Invalid text buffer provided (null).");
        return ESP_ERR_INVALID_ARG;
    }

    int bytes_sent = esp_websocket_client_send_text(client, text, strlen(text), pdMS_TO_TICKS(ESP_WEBSOCKET_CLIENT_SEND_TIMEOUT_MS));
    if (bytes_sent < 0) {
        ESP_LOGE(TAG, "Error sending text message via WebSocket");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Send a predefined heartbeat message.
 * @return ESP_OK on success, or ESP_FAIL on failure.
 * Wrapper around websocket_send_text for sending 
 * keep-alive messages.
 */
esp_err_t websocket_send_heartbeat(void) {
    return websocket_send_text("{\"type\":\"heartbeat\"}");
}

/**
 * @brief Check if the WebSocket client is connected.
 * @return True if connected, false otherwise.
 * Query the connection status via the event group.
 */
bool is_websocket_connected(void) {
    if (s_client_event_group == NULL) return false;
    return (xEventGroupGetBits(s_client_event_group) & WEBSOCKET_CONNECTED_BIT) != 0;
}