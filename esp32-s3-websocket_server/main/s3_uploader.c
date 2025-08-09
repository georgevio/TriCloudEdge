#include "s3_uploader.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "storage_manager.h"
#include "face_database.h"
#include "string.h"
#include "cJSON.h"
#include "utils/config.h"
#include "utils/time_sync.h"

static const char* TAG = "S3_UPLOADER";

// Struct to hold response data for the event handler
typedef struct {
    char* buffer;
    int buffer_size;
    int data_len;
} http_response_data_t;

// Event handler for the HTTP client.
esp_err_t _http_event_handler(esp_http_client_event_t* evt) {
    http_response_data_t* response_data = (http_response_data_t*)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (response_data->data_len + evt->data_len < response_data->buffer_size) {
            memcpy(response_data->buffer + response_data->data_len, evt->data, evt->data_len);
            response_data->data_len += evt->data_len;
        }
        else {
            ESP_LOGE(TAG, "HTTP response buffer overflow");
            return ESP_FAIL;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP connection finished.");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP disconnected.");
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t s3_uploader_get_presigned_url(const char* filename, char* presigned_url, size_t max_url_len) {
    if (!is_time_synchronized()) {
        ESP_LOGE(TAG, "Time is not synchronized. Cannot get a pre-signed URL.");
        return ESP_FAIL;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://%s%s?filename=%s", API_GATEWAY_HOST, API_GATEWAY_PATH, filename);
    ESP_LOGD(TAG, "Requesting pre-signed URL from: %s", url);

    char response_buffer[2048] = { 0 };
    http_response_data_t response_data = {
        .buffer = response_buffer,
        .buffer_size = sizeof(response_buffer),
        .data_len = 0
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .cert_pem = _binary_AmazonRootCA1_pem_start,
        .event_handler = _http_event_handler,
        .user_data = &response_data,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_err_t final_err = ESP_FAIL;

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            if (response_data.data_len > 0) {
                cJSON* root = cJSON_Parse(response_data.buffer);
                if (root) {
                    cJSON* upload_url = cJSON_GetObjectItem(root, "uploadUrl");
                    if (cJSON_IsString(upload_url) && (upload_url->valuestring != NULL)) {
                        strncpy(presigned_url, upload_url->valuestring, max_url_len - 1);
                        presigned_url[max_url_len - 1] = '\0';
                        final_err = ESP_OK;
                    }
                    else {
                        ESP_LOGE(TAG, "Missing or invalid 'uploadUrl' in JSON");
                    }
                    cJSON_Delete(root);
                }
                else {
                    ESP_LOGE(TAG, "Failed to parse JSON response");
                }
            }
            else {
                ESP_LOGE(TAG, "Got status 200 but failed to read HTTP response body");
            }
        }
        else {
            ESP_LOGE(TAG, "HTTP GET request failed with status code: %d", status_code);
        }
    }
    else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return final_err;
}

esp_err_t s3_uploader_upload_by_url(const char* s3_url, const uint8_t* data, size_t data_len, const char* content_type) {
    ESP_LOGD(TAG, "Uploading %zu bytes to S3...", data_len);

    esp_http_client_config_t config = {
        .url = s3_url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = 15000,
        .cert_pem = _binary_AmazonRootCA1_pem_start,
        .skip_cert_common_name_check = true,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for upload");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int write_len = esp_http_client_write(client, (const char*)data, data_len);
    if (write_len < 0) {
        ESP_LOGE(TAG, "Failed to write data to HTTP stream");
        err = ESP_FAIL;
    }
    else {
        int status_code = esp_http_client_fetch_headers(client);
        if (status_code < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed: %d", status_code);
            err = ESP_FAIL;
        }
        else {
            status_code = esp_http_client_get_status_code(client);
            if (status_code == 200) {
                ESP_LOGD(TAG, "File uploaded successfully!");
                err = ESP_OK;
            }
            else {
                ESP_LOGE(TAG, "S3 upload failed. Code %d", status_code);
                err = ESP_FAIL;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t s3_uploader_test_connectivity(void) {

    if (!is_time_synchronized()) {
        ESP_LOGE(TAG, "Time is not synchronized. Cannot test S3 connection.");
        return ESP_FAIL;
    }
    char presigned_url[2048];
    const char* test_filename = "connection_test.bin";
    ESP_LOGD(TAG, "Testing connection to AWS API Gateway...");
    return s3_uploader_get_presigned_url(test_filename, presigned_url, sizeof(presigned_url));
}

esp_err_t s3_uploader_test_upload(void) {
    if (!is_time_synchronized()) {
        ESP_LOGE(TAG, "Time is not synchronized. Cannot perform S3 upload test.");
        return ESP_FAIL;
    }

    face_record_t* records = NULL;
    int record_count = 0;
    esp_err_t err = ESP_FAIL;
    char presigned_url[2048];
    char* embedding_data = NULL;

    ESP_LOGI(TAG, "Initializing DB for full S3 upload test...");
    if (database_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize database for S3 test.");
        return ESP_FAIL;
    }

    if (database_get_all_faces(&records, &record_count) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get records from database.");
        goto cleanup;
    }

    if (record_count == 0) { // not an error, but cannot test the upload!
        ESP_LOGW(TAG, "No face records in database. S3 upload test skipped.");
        err = ESP_OK;
        goto cleanup;
    }

    face_record_t first_record = records[0];
    size_t embedding_len = 0;

    if (storage_read_file(first_record.embedding_file, &embedding_data, &embedding_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read embedding file %s.", first_record.embedding_file);
        goto cleanup;
    }

    const char* upload_filename = "test_embedding_person0.bin";

    if (s3_uploader_get_presigned_url(upload_filename, presigned_url, sizeof(presigned_url)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get pre-signed URL for S3 upload.");
        goto cleanup;
    }

    err = s3_uploader_upload_by_url(presigned_url, (const uint8_t*)embedding_data, embedding_len, "application/octet-stream");

cleanup:
    if (embedding_data) {
        free(embedding_data);
    }
    database_deinit();
    return err;
}