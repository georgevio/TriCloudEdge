/* 
 * Optional module for the WebSocket keep-alive task.
 * Sends periodic keepalive messages to websocket.
 * Bibliography mentions necessity over multiple switches,
 * Firewalls, etc.
 * HASNT PROVED ITS NECESSITY. 
 * Can be disabled in config.h
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the heartbeat module with the application event group.
 * @param event_group Handle to the main application event group.
 */
void heartbeat_init(EventGroupHandle_t event_group);

/**
 * @brief Periodically send a heartbeat message to the WebSocket.
 * @param pvParameters Unused task parameters.
 */
void heartbeat_task(void* pvParameters);

#ifdef __cplusplus
}
#endif

#endif // HEARTBEAT_H