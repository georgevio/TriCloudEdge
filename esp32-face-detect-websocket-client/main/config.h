
#ifndef CONFIG_H
#define CONFIG_H

#include "secret.h"  // Has to be created. ALL SENSITIVE CONFGIS ARE THERE!
#include "esp_log.h" // Required for ESP_LOG_INFO


// Unified Log level for all components. Can be overridden per-component in app_main.cpp
// LEVELS: ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE
#define DEFAULT_SYSTEM_LOG_LEVEL ESP_LOG_INFO

/* Client periodically beeps the websocket server to keep the connection alive. */
#define HEARTBEAT_ON 1
#define HEARTBEAT_INTERVAL_S 300 

/* Face Detection & Image Sending parameters */
#define POST_DETECTION_COOLDOWN_S 20 // Stop camera for XX seconds after detection. Prevents duplicate images
#define FACE_CROP_MARGIN_PIXELS 20   // Margin in pixels to add around the detected face
#define FRAME_QUEUE_SIZE 2           // Size of the frames queue for processing
#define WEBSOCKET_CHUNK_SIZE 8192    // Max size for each chunk of a binary WebSocket message
#define SERVER_ACK_TIMEOUT_MS 5000   // Timeout to wait for a frame ACK from the server

/* If automatic settings fail to (easily) detect a face, 
 * set to 1 and experiment with manual settings in app_main.cpp 
 * It seems that there is a big difference depending on ambient conditions!
 */
#define MANUAL_CAMERA_TUNING 0

// Bit flags, NOT integer values. Do not change them unless you understand event groups.
#define WIFI_CONNECTED_BIT        (1 << 0)
#define WEBSOCKET_CONNECTED_BIT   (1 << 1)
#define FRAME_ACK_BIT             (1 << 2)

/* Time & Timezone setting. 
 * For AWS, UTC is strongly recommended.
 * A list of timezone strings:
 * https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
 * 
 * Tried to automate the TIMEZONE, but it is complicated and rather
 * computation consuming, so kept it manual: CET-1 Central Europe.
 */
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3" // UTC+1 or CET

#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

#endif // CONFIG_H
