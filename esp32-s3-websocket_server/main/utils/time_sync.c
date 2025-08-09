#include "time_sync.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "config.h" // timezone & NTP servers

static const char* TAG = "TIME_SYNC";

// Event group to signal when time sync is complete
static EventGroupHandle_t s_time_event_group;
static const int TIME_SYNC_DONE_BIT = BIT0;

// Static current status
static bool s_time_synced = false;

// SNTP callback function, called when time is synchronized
static void time_sync_notification_cb(struct timeval* tv) {
    ESP_LOGD(TAG, "Network time synchronized callback triggered.");
    // Set timezone from config.h
    setenv("TZ", TIMEZONE, 1);
    tzset();
    s_time_synced = true;
    xEventGroupSetBits(s_time_event_group, TIME_SYNC_DONE_BIT);
}

esp_err_t time_sync_init(char* time_str_buf, size_t buf_len) {
    s_time_event_group = xEventGroupCreate();
    if (s_time_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    ESP_LOGD(TAG, "Waiting for time synchronization...");
    // Wait for the callback to set the event bit. Timeout after 15 seconds.
    EventBits_t bits = xEventGroupWaitBits(s_time_event_group, TIME_SYNC_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    // Cleanup the event group as no longer needed
    vEventGroupDelete(s_time_event_group);
    s_time_event_group = NULL;

    if (bits & TIME_SYNC_DONE_BIT) {
        // Fill the buffer with the human-readable time string
        get_human_readable_time_string(time_str_buf, buf_len);
        return ESP_OK;
    }
    else {
        ESP_LOGE(TAG, "Failed to synchronize time within 15 seconds.");
        esp_sntp_stop();
        s_time_synced = false;
        return ESP_FAIL;
    }
}

bool is_time_synchronized(void) {
    return s_time_synced;
}

void get_human_readable_time_string(char* buf, size_t buf_len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Format: Weekday, Month Day, Year HH:MM:SS (e.g., Thursday, July 10, 2025 09:55:34)
    strftime(buf, buf_len, "%A, %B %d, %Y %H:%M:%S", &timeinfo);
}

void get_utc_time_string(char* buf, size_t buf_len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    // Use gmtime_r for UTC regardless of local timezone setting
    gmtime_r(&now, &timeinfo);
    // Format: YYYY-MM-DDTHH:MM:SSZ (ISO 8601)
    strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
}