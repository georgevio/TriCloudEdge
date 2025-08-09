/**
 * @file storage_manager.c
 * @version 31
 * @brief Implementation for NVS and SPIFFS storage management.
 */
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h> 

static const char* TAG = "STORAGE";

esp_err_t storage_init(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    ESP_LOGI(TAG, "SPIFFS mounted successfully at %s", conf.base_path);
    return ESP_OK;
}

esp_err_t storage_read_file(const char *path, char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", path);
        *out_buf = NULL;
        *out_len = 0;
        return ESP_FAIL;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat file: %s", path);
        fclose(f);
        *out_buf = NULL;
        *out_len = 0;
        return ESP_FAIL;
    }

    *out_len = st.st_size;
    *out_buf = (char *)malloc(*out_len + 1);
    if (*out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for file content.");
        fclose(f);
        *out_len = 0;
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = fread(*out_buf, 1, *out_len, f);
    if (bytes_read != *out_len) {
        ESP_LOGW(TAG, "Read less bytes than file size for %s (read %d of %d)", path, bytes_read, *out_len);
        *out_len = bytes_read;
    }
    (*out_buf)[bytes_read] = '\0';
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing (text): %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "%s", content);
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_write_file_binary(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for binary writing: %s", path);
        return ESP_FAIL;
    }
    size_t bytes_written = fwrite(data, 1, len, f);
    fclose(f);
    if (bytes_written != len) {
        ESP_LOGE(TAG, "Failed to write all bytes to file %s (wrote %d of %d)", path, bytes_written, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_delete_file(const char *path) {
    int ret = remove(path);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to delete file %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "File %s deleted.", path);
    return ESP_OK;
}