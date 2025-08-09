#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t storage_init(void);
esp_err_t storage_read_file(const char *path, char **out_buf, size_t *out_len);
esp_err_t storage_write_file(const char *path, const char *content);
esp_err_t storage_write_file_binary(const char *path, const uint8_t *data, size_t len);
esp_err_t storage_delete_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_MANAGER_H