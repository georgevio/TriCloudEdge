#include "message_handler.h"
#include "esp_log.h"
#include "cJSON.h"
#include "utils/config.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "MSG_HANDLER";

/**
 * @brief Parse incoming text messages from the WebSocket server.
 * @param message A null-terminated string received from the server.
 * @param event_group handle to signal events , e.g., frame ACKs.
 * Decodes server messages, handling response types such as
 * connection info, frame acknowledgements, and face recognition results.
 *
 * This function should be the only one that needs to be extended
 * with handling new, updated messages...
 */
void message_handler_process(const char *message, EventGroupHandle_t event_group) {
    if (message == NULL) {
        return;
    }

    // Check for simple, non-JSON messages first
    if (strstr(message, "frame_ack") != NULL) {
        ESP_LOGD(TAG, "Got frame ACK.");
        if (event_group) {
            xEventGroupSetBits(event_group, FRAME_ACK_BIT);
        }
        return; // Message handled, exit
    }
    if (strstr(message, "Welcome, client fd") != NULL) {
        int client_fd = 0;
        if (sscanf(message, "Welcome, client fd %d!", &client_fd) == 1) {
            ESP_LOGI(TAG, "Server assigned client ID %d", client_fd);
        } else {
            ESP_LOGI(TAG, "Received welcome message: %s", message);
        }
        return; // Message handled, exit
    }

    // TODO: The logic below is very bad. NEEDS COMPLETE REFORM
    
    // If not a simple string, try to parse it as JSON
    cJSON *root = cJSON_Parse(message);
    if (root) {
        const char* person_name = NULL;

        // Check for the "name" key (from local recognition)
        cJSON *name_item = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(name_item) && name_item->valuestring != NULL) {
            person_name = name_item->valuestring;
        }
        // If "name" not found, check for the "result" key (from AWS)
        else {
            cJSON *result_item = cJSON_GetObjectItem(root, "result");
            if (cJSON_IsString(result_item) && result_item->valuestring != NULL) {
                person_name = result_item->valuestring;
            }
        }

        // If a name was found (from either key), process and display it
        if (person_name) {
            if (strcmp(person_name, "Bill Gates") == 0) {
                ESP_LOGW(TAG, "****************************************");
                ESP_LOGW(TAG, "  Bill is at the gate, OPEN THE DOOR!");
                ESP_LOGW(TAG, "****************************************");
            } else if (strcmp(person_name, "Face not Recognized") == 0) {
                ESP_LOGW(TAG, "Recognition status: %s", person_name);
            } else {
                ESP_LOGI(TAG, "\033[1;36m***************************************\033[0m");
                ESP_LOGI(TAG, "\033[1;36m Visitor recognized: %s \033[0m", person_name);
                ESP_LOGI(TAG, "\033[1;36m***************************************\033[0m");
            }
        }
        cJSON_Delete(root);
    } else {
        // If JSON parsing fails, log the raw string for debugging
        ESP_LOGW(TAG, "Received unhandled/malformed text message: %s", message);
    }
}