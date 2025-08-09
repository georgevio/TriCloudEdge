/**
 * @file mqtt.c
 * @brief MQTT client module implementation for AWS IoT
 * Mostly standarized code provided for AWS connections. No alterations
 * Handles MQTT client initialization, connection, and publishing to AWS IoT.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "mqtt.h"
#include "config.h" // Includes secret.h with certificates
#include "cJSON.h" 

static const char* TAG = "MQTT";
static bool mqtt_connected_status = false;
static esp_mqtt_client_handle_t g_client = NULL; // Global client handle


// Pointer for the AWS Rekognition result callback
static void (*rekognition_result_callback)(const char* message) = NULL;

// Register the callback
void mqtt_register_rekognition_callback(void (*callback)(const char* message)) {
    rekognition_result_callback = callback;
}

static void (*connection_state_callback)(bool connected, esp_mqtt_client_handle_t client) = NULL;

// function implementation
void mqtt_register_connection_callback(void (*callback)(bool connected, esp_mqtt_client_handle_t client)) {
    connection_state_callback = callback;
}

// Update the MQTT event handler function to use the callback
static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGD(TAG, "MQTT connected to AWS IoT!");
        mqtt_connected_status = true;

        /* ALL DIFFERENT TOPIC SUBSCRIPTIONS HERE. Can add more */

        // Subscribe to command topic
        const char* command_topic = MQTT_TOPIC_DEVICE "/commands";
        int msg_id_cmd = esp_mqtt_client_subscribe(client, command_topic, 0);
        ESP_LOGI(TAG, "Subscribed to %s", command_topic);
        ESP_LOGD(TAG, "Message ID (msg_id) = %d", msg_id_cmd);
        // Subscribe to S3 notification topic
        int msg_id_s3 = esp_mqtt_client_subscribe(client, MQTT_TOPIC_S3_NOTIFY, 1);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_S3_NOTIFY);
        ESP_LOGD(TAG, "Message ID (msg_id) = %d", msg_id_s3);
        // Subscribe to status topic
        int msg_id_status = esp_mqtt_client_subscribe(client, MQTT_TOPIC_STATUS, 1);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_STATUS );
        ESP_LOGD(TAG, "Message ID (msg_id) = %d", msg_id_status);
        // Ssubscription for AWS Rekognition results
        int msg_id_rekognition = esp_mqtt_client_subscribe(client, MQTT_TOPIC_REKOGNITION_RESULT, 1);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_REKOGNITION_RESULT);
        ESP_LOGD(TAG, "Message ID (msg_id) = %d", msg_id_rekognition);
#if MQTT_PUBLISH_INIT_MESSAGE
        // Send initialization message when connected
        char init_message[256];
        char client_id[64];

        // Get the local device client ID - Config value or MAC address as fallback
#ifdef AWS_IOT_CLIENT_ID
        snprintf(client_id, sizeof(client_id), "%s", AWS_IOT_CLIENT_ID);
#else
    // Get MAC address as client ID if not specified otherwise
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(client_id, sizeof(client_id), "ESP32S3_%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
        // initialization message - fixed timestamp format specifier
        snprintf(init_message, sizeof(init_message),
            "{\"message\":\"Device connected\",\"client_id\":\"%s\",\"topic\":\"%s\",\"timestamp\":%llu}",
            client_id, MQTT_TOPIC_BASE, (unsigned long long)(esp_timer_get_time() / 1000));

        // Publish to a dedicated connection status topic
        ESP_LOGI(TAG, "Sending initialization message: %s", init_message);
        mqtt_publish_message(client, MQTT_TOPIC_BASE "/status/connect", init_message, 1, 0);
#endif

        // Call the connection callback if registered
        if (connection_state_callback != NULL) {
            connection_state_callback(true, client);
        }

        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        mqtt_connected_status = false;  // Update connection status

        // Call the connection callback if registered
        if (connection_state_callback != NULL) {
            connection_state_callback(false, client);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT message published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGD(TAG, "MQTT data received on topic: %.*s", event->topic_len, event->topic);

        // Check if the message is on the S3 notification topic
        if (strncmp(event->topic, MQTT_TOPIC_S3_NOTIFY, event->topic_len) == 0) {
            char* json_string = (char*)malloc(event->data_len + 1);
            if (json_string == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for JSON string");
                break;
            }
            memcpy(json_string, event->data, event->data_len);
            json_string[event->data_len] = '\0';

            cJSON* root = cJSON_Parse(json_string);
            if (root) {
                cJSON* status = cJSON_GetObjectItem(root, "status");
                cJSON* bucket = cJSON_GetObjectItem(root, "bucket");
                cJSON* filename = cJSON_GetObjectItem(root, "filename");

                if (cJSON_IsString(status)) { ESP_LOGD(TAG, "S3 Status: %s", status->valuestring); }
                if (cJSON_IsString(bucket)) { ESP_LOGD(TAG, "S3 Bucket: %s", bucket->valuestring); }
                if (cJSON_IsString(filename)) { ESP_LOGI(TAG, "S3 Filename Received: %s", filename->valuestring); }

                cJSON_Delete(root);
            }
            free(json_string);

        }
        else if (strncmp(event->topic, MQTT_TOPIC_REKOGNITION_RESULT, event->topic_len) == 0) {
            // Handle the Rekognition result
            char* json_string = (char*)malloc(event->data_len + 1);
            if (json_string == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for JSON string");
                break;
            }
            memcpy(json_string, event->data, event->data_len);
            json_string[event->data_len] = '\0';

            cJSON* root = cJSON_Parse(json_string);
            if (root) {
                cJSON* result = cJSON_GetObjectItem(root, "result");
                if (cJSON_IsString(result)) {
                    //ESP_LOGW(TAG, "Rekognition Result: %s", result->valuestring);
                    ESP_LOGI(TAG, "\033[1;36m           Rekognition Result: %s\033[0m", result->valuestring);
                    // JSON compatible callback from rekognition
                    if (rekognition_result_callback != NULL) {
                        rekognition_result_callback(json_string);
                    }
                }
                cJSON_Delete(root);
            }
            free(json_string);

        }
        else {
            // For any other topic, just print the received data
            ESP_LOGI(TAG, "MQTT data received on unknown topic:");
            ESP_LOGI(TAG, "  Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "  Data: %.*s", event->data_len, event->data);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error occurred");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "  Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "  Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        ESP_LOGD(TAG, "MQTT event id:%ld", (long)event_id);
        break;
    }
}

esp_mqtt_client_handle_t mqtt_aws_init(void)
{
    // Make sure certificate is loaded, just in case
    ESP_LOGD(TAG, "Root CA Preview: %.20s", _binary_AmazonRootCA1_pem_start);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = AWS_IOT_ENDPOINT,
            .verification.certificate = _binary_AmazonRootCA1_pem_start,
        },
        .credentials = {
            .authentication = {
                .certificate = _binary_new_certificate_pem_start,
                .key = _binary_new_private_key_start,
            },
            .client_id = AWS_IOT_CLIENT_ID
        },
        .session = {
            .last_will = {
                .topic = MQTT_TOPIC_STATUS,
                .msg = "offline",
                .qos = 1,
            }
        }
    };

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return NULL;
    }

    esp_err_t err = esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
        return NULL;
    }

    return g_client;
}

esp_err_t mqtt_start(esp_mqtt_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGD(TAG, "MQTT client started");
    }

    return err;
}

int mqtt_publish_message(esp_mqtt_client_handle_t client, const char* topic, const char* data, int qos, int retain)
{
    if (client == NULL || topic == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid arguments for mqtt_publish_message");
        return -1;
    }

    ESP_LOGD(TAG, "Publishing to topic: %s", topic);
    ESP_LOGD(TAG, "Message data: %s", data);
    ESP_LOGD(TAG, "QoS: %d, Retain: %d", qos, retain);

    int msg_id = esp_mqtt_client_publish(client, topic, data, 0, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to topic %s", topic);
    }
    else {
        ESP_LOGD(TAG, "Published to topic %s, msg_id=%d", topic, msg_id);
    }
    return msg_id;
}


bool mqtt_is_connected(void)
{
    return mqtt_connected_status;
}

esp_mqtt_client_handle_t get_mqtt_client(void) {
    return g_client;
}
