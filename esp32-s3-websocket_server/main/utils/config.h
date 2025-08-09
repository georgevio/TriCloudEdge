#ifndef CONFIG_H
#define CONFIG_H

#include "../certificates/secret.h"  // sensitive configuration

/**********LOGGING CONFIGURATION ************
 * IMPROTANT: To be able to view messages above INFO LEVEL, 
 * for example VERBOSE, do:
 * idf.py menuconfig -> Component config--->Log output--->
 * option (Info) Maximum log verbosity -> Change to (Verbose) Verbose.
 * Save & Exit.
 */


 /* 
  * Set the default log level if not otherwise set, for
  * system level applications.
  * IT ONLY WORKS AFTER THE main_app HAS EXECUTED!
  * User space apps (e.g., MAIN, WIFI, S3_UPLOADER, etc.) 
  * will remain visible, and can be indicitually adjusted.
  * Available levels:
  * ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, 
  * ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE
  */ 
#define DEFAULT_SYSTEM_LOG_LEVEL ESP_LOG_WARN


/* MODULE SPECIFIC FLAGS */
#define MQTT_ENABLED 1
#define MQTT_PUBLISH_INIT_MESSAGE 0 

#define WIFI_ENABLED 1
#define WIFI_MAXIMUM_RETRY 5

#define WEBSOCKET_ENABLED 1
#define WEBSOCKET_PORT 80 

/* Threshold for face comparison. 
 * NEEDS DISCUSSION AND TUNING! 
 * DEPENDS HEAVILY ON AMBIENT CONDITIONS!
 */
#define COSINE_SIMILARITY_THRESHOLD 0.75f // 0.95 IS VERY VERY DIFFICULT!

// image_processor.cpp
#define ENABLE_ENROLLMENT 0 // USE ONLY TO INSERT NEW FACES, WITH DB ERASE ON STARTUP
#define SEND_UNKNOWN_FACES_TO_AWS 1 // Send unknowns to AWS S3 for further processing. 

/* S3 Uploader Startup Test Configuration. Advised to start with 2! */
// 0: No test performed on startup.
// 1: (Default) Check connection ONBLY to AWS API Gateway.
// 2: File upload to S3 for full test (It was very difficult to connect to AWS S3 API!)
#define S3_STARTUP_TEST_MODE 1

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
