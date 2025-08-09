#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the AWS IoT MQTT client.
 * @return Handle to the MQTT client, or NULL on failure.
 */
esp_mqtt_client_handle_t mqtt_aws_init(void);

/**
 * @brief Starts the MQTT client connection.
 * @param client Handle to the MQTT client.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t mqtt_start(esp_mqtt_client_handle_t client);

/**
 * @brief Publishes a message to a specific MQTT topic.
 * @param client Handle to the MQTT client.
 * @param topic The topic to publish to.
 * @param data The message payload.
 * @param qos Quality of Service level.
 * @param retain Retain flag.
 * @return Message ID on success, or a negative value on failure.
 */
int mqtt_publish_message(esp_mqtt_client_handle_t client, const char* topic, const char* data, int qos, int retain);

/**
 * @brief Checks if the MQTT client is currently connected.
 * @return True if connected, false otherwise.
 */
bool mqtt_is_connected(void);

/**
 * @brief Gets the global MQTT client handle.
 * @return Handle to the MQTT client.
 */
esp_mqtt_client_handle_t get_mqtt_client(void);

/**
 * @brief Registers a callback function for when a Rekognition result is received.
 * @param callback The function pointer takes a const char* (the JSON message) as an argument.
 */
void mqtt_register_rekognition_callback(void (*callback)(const char* message));

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
