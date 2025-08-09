/* Code was taken from espidff examples and internet provided. 
 * Functionalities were used as-is.
 * The only job that the websocket server does, is upon receipt 
 * of an imnage, pass it to the image_processor module, and free 
 * the memory to be ready for the next one.
 * ADVICE: Dont insert other intelligence here, the websocket 
 * server has to remain agnostic.
 */

#include "esp_http_server.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <sys/param.h>
#include <string.h>
#include <stdlib.h>
#include <vector> 
#include "esp_heap_caps.h" 

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "websocket_server.h"
#include "utils/config.h"
#include "cJSON.h"
#include "image_processor.h" // pass the incoming image to the image processor. No other function on image here

#ifndef WEBSOCKET_PORT
#define WEBSOCKET_PORT 80
#endif

static const char* TAG = "WEBSOCKET_SERVER";

#define MAX_WEBSOCKET_CLIENTS CONFIG_LWIP_MAX_ACTIVE_TCP

//includes image dimensions because the cropped image from client is variable size
typedef struct {
    uint8_t* buffer;
    size_t total_size;
    size_t received_size;
    bool is_receiving;
    uint32_t id;
    int width;
    int height;
    // face bounding box coordinates
    int face_x;
    int face_y;
    int face_w;
    int face_h;
    std::vector<int> keypoints; // store received keypoints
} frame_receive_state_t;

typedef struct {
    int fd;
    bool active;
} ws_client_t;

static httpd_handle_t server_handle = NULL;
static ws_client_t ws_clients[MAX_WEBSOCKET_CLIENTS];
static frame_receive_state_t client_frame_states[MAX_WEBSOCKET_CLIENTS];

static void ws_async_send(void* arg);
static esp_err_t websocket_handler(httpd_req_t* req);
static void reset_client_frame_state(int fd);
static int find_client_index_by_fd(int fd);

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = websocket_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = NULL, // annoying warning
    .supported_subprotocol = NULL      // annoying warning
};

static int find_client_index_by_fd(int fd) {
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        if (ws_clients[i].active && ws_clients[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static void reset_client_frame_state(int fd) {
    int client_index = find_client_index_by_fd(fd);
    if (client_index != -1) {
        if (client_frame_states[client_index].buffer) {
            ESP_LOGD(TAG, "Freeing frame buffer for client %d", fd);
            free(client_frame_states[client_index].buffer);
            ESP_LOGD(TAG, "Free heap after buffer free (reset_state): %" 
                    PRIu32, (uint32_t)heap_caps_get_free_size(
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        client_frame_states[client_index].buffer = NULL;
        client_frame_states[client_index].is_receiving = false;
        client_frame_states[client_index].received_size = 0;
        client_frame_states[client_index].total_size = 0;
        client_frame_states[client_index].id = 0;
        client_frame_states[client_index].width = 0;
        client_frame_states[client_index].height = 0;
        client_frame_states[client_index].face_x = 0;
        client_frame_states[client_index].face_y = 0;
        client_frame_states[client_index].face_w = 0;
        client_frame_states[client_index].face_h = 0;
        client_frame_states[client_index].keypoints.clear();
        ESP_LOGD(TAG, "Client frame state reset for fd %d", fd);
    }
}

static void server_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {
    if (event_base == ESP_HTTP_SERVER_EVENT) {
        if (event_id == HTTP_SERVER_EVENT_DISCONNECTED) {
            int sockfd = *((int*)event_data);
            ESP_LOGI(TAG, "Client disconnected with fd %d", sockfd);
            reset_client_frame_state(sockfd);
            int client_index = find_client_index_by_fd(sockfd);
            if (client_index != -1) {
                ws_clients[client_index].active = false;
                ws_clients[client_index].fd = -1;
            }
        }
    }
}

static esp_err_t websocket_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Client connected with fd %d", httpd_req_to_sockfd(req));
        int sockfd = httpd_req_to_sockfd(req);
        int client_index = -1;
        for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
            if (!ws_clients[i].active) {
                client_index = i;
                break;
            }
        }

        if (client_index != -1) {
            ws_clients[client_index].fd = sockfd;
            ws_clients[client_index].active = true;
            reset_client_frame_state(sockfd); // Reset state on new connection
            ESP_LOGD(TAG, "Client fd: %d added to list at index %d", sockfd, client_index);
            char welcome_msg[64];
            snprintf(welcome_msg, sizeof(welcome_msg), "Welcome, client fd %d!", sockfd);
            websocket_server_send_text_client(sockfd, welcome_msg);
        }
        else {
            ESP_LOGW(TAG, "Could not add client fd %d, client list full?", sockfd);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t* buf = NULL;
    int client_index = find_client_index_by_fd(httpd_req_to_sockfd(req));

    if (client_index < 0) {
        ESP_LOGE(TAG, "Request from unknown client fd %d, ignoring.", httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }

    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // get type and length of frame
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            return ESP_OK; // No data, just timeout, no error
        }
        ESP_LOGE(TAG, "httpd_ws_recv_frame (peek) error %d: %s", ret, esp_err_to_name(ret));
        reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset state on any receivd error
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0) {
            buf = (uint8_t*)calloc(1, ws_pkt.len + 1); // +1 = null terminator
            if (!buf) {
                ESP_LOGE(TAG, "Failed to allocate memory for text payload.");
                return ESP_ERR_NO_MEM;
            }
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len); // Read the actual payload
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "httpd_ws_recv_frame (text) error %d: %s", ret, esp_err_to_name(ret));
                free(buf);
                reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset state on text frame error
                return ret;
            }
            buf[ws_pkt.len] = '\0'; // Null-terminate the string

            cJSON* root = cJSON_Parse((const char*)buf);
            if (!root) { // check for cJSON parsing failure
                ESP_LOGE(TAG, "Failed to parse JSON from text message: %s", (const char*)buf);
                free(buf);
                return ESP_FAIL;
            }

            cJSON* type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                // websocket clients can potentially send a heartbeat message, in adjustable intervals
                if (strcmp(type->valuestring, "heartbeat") == 0) {
                    ESP_LOGD(TAG, "Heartbeat received from fd %d", httpd_req_to_sockfd(req));
                    const char* pong_msg = "{\"type\":\"heartbeat_ack\"}";
                    websocket_server_send_text_client(httpd_req_to_sockfd(req), pong_msg);

                }
                else if (strcmp(type->valuestring, "frame_start") == 0) {
                    // If already receiving a frame, reset state for this client before starting a new one
                    if (client_frame_states[client_index].is_receiving) {
                        ESP_LOGW(TAG, "Got a frame_start while receiving for fd %d. Resetting state.", 
                                httpd_req_to_sockfd(req));
                        reset_client_frame_state(httpd_req_to_sockfd(req));
                    }
                    cJSON* size = cJSON_GetObjectItem(root, "size");
                    cJSON* id = cJSON_GetObjectItem(root, "id");
                    cJSON* width = cJSON_GetObjectItem(root, "width");
                    cJSON* height = cJSON_GetObjectItem(root, "height");
                    cJSON* box_x = cJSON_GetObjectItem(root, "box_x");
                    cJSON* box_y = cJSON_GetObjectItem(root, "box_y");
                    cJSON* box_w = cJSON_GetObjectItem(root, "box_w");
                    cJSON* box_h = cJSON_GetObjectItem(root, "box_h");
                    cJSON* keypoints_array = cJSON_GetObjectItem(root, "keypoints");

                    if (cJSON_IsNumber(size) && cJSON_IsNumber(id) && cJSON_IsNumber(width) && cJSON_IsNumber(height) &&
                        cJSON_IsNumber(box_x) && cJSON_IsNumber(box_y) && cJSON_IsNumber(box_w) && cJSON_IsNumber(box_h) &&
                        cJSON_IsArray(keypoints_array) && cJSON_GetArraySize(keypoints_array) == 10) {

                        client_frame_states[client_index].total_size = size->valueint;
                        client_frame_states[client_index].id = id->valueint;
                        client_frame_states[client_index].width = width->valueint;
                        client_frame_states[client_index].height = height->valueint;
                        client_frame_states[client_index].face_x = box_x->valueint;
                        client_frame_states[client_index].face_y = box_y->valueint;
                        client_frame_states[client_index].face_w = box_w->valueint;
                        client_frame_states[client_index].face_h = box_h->valueint;

                        // Parse keypoints array
                        client_frame_states[client_index].keypoints.clear(); // Clear any old data
                        for (int i = 0; i < cJSON_GetArraySize(keypoints_array); ++i) {
                            cJSON* item = cJSON_GetArrayItem(keypoints_array, i);
                            if (cJSON_IsNumber(item)) {
                                client_frame_states[client_index].keypoints.push_back(item->valueint);
                            }
                            else {
                                ESP_LOGE(TAG, "Keypoints array contains non-numeric item for fd %d. Raw: %s", httpd_req_to_sockfd(req), (const char*)buf);
                                reset_client_frame_state(httpd_req_to_sockfd(req));
                                cJSON_Delete(root);
                                free(buf);
                                return ESP_FAIL; // Abort on invalid keypoint type
                            }
                        }

                        client_frame_states[client_index].received_size = 0;
                        client_frame_states[client_index].buffer = (uint8_t*)malloc(size->valueint);
                        if (client_frame_states[client_index].buffer) {
                            client_frame_states[client_index].is_receiving = true;
                            ESP_LOGI(TAG, "\033[1;33m↓↓↓ New incoming image ↓↓↓\033[0m");
                            ESP_LOGD(TAG, "Incoming image: Size: %d, Dimensions: %dx%d, Box: [%d,%d,%d,%d], Keypoints size: %zu",
                                (int)size->valueint, (int)width->valueint, (int)height->valueint,
                                client_frame_states[client_index].face_x, client_frame_states[client_index].face_y,
                                client_frame_states[client_index].face_w, client_frame_states[client_index].face_h,
                                client_frame_states[client_index].keypoints.size());

                            ESP_LOGD(TAG, "Free heap after buffer alloc: %" PRIu32, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                            // Send acknowledgment for frame_start
                            const char* ack_msg = "{\"type\":\"frame_start_ack\"}";
                            websocket_server_send_text_client(httpd_req_to_sockfd(req), ack_msg);
                        }
                        else {
                            ESP_LOGE(TAG, "Failed to allocate buffer for frame ID %u!", (unsigned int)id->valueint);
                            reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset on buffer allocation failure
                        }
                    }
                    else {
                        ESP_LOGE(TAG, "Invalid frame_start JSON fields (missing/invalid numbers or keypoints array size != 10) received from fd %d. JSON: %s", httpd_req_to_sockfd(req), (const char*)buf);
                        reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset on invalid JSON fields
                    }

                }
                else if (strcmp(type->valuestring, "frame_end") == 0) {
                    if (client_frame_states[client_index].is_receiving) {
                        if (client_frame_states[client_index].received_size == client_frame_states[client_index].total_size) {
                            ESP_LOGI(TAG, "File transfer complete, size: %d", (int)client_frame_states[client_index].total_size);

                            ESP_LOGD(TAG, "Ready to call image_processor_handle_new_image with:");
                            ESP_LOGD(TAG, "  Buffer Addr: %p", client_frame_states[client_index].buffer);
                            ESP_LOGD(TAG, "  Buffer Len: %zu", client_frame_states[client_index].total_size);
                            ESP_LOGD(TAG, "  Cropped Img Dims (Width x Height): %d x %d",
                                client_frame_states[client_index].width, client_frame_states[client_index].height);
                            ESP_LOGD(TAG, "  Original Face Box (x,y,w,h): %d,%d,%d,%d",
                                client_frame_states[client_index].face_x, client_frame_states[client_index].face_y,
                                client_frame_states[client_index].face_w, client_frame_states[client_index].face_h);
                            ESP_LOGD(TAG, "  Keypoints Count: %zu", client_frame_states[client_index].keypoints.size());
                            ESP_LOGD(TAG, "  Free heap before image_processor: %" PRIu32 "", (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                            /* Pass the image to image_processor. NO OTHER JOB HERE */
                            image_processor_handle_new_image(
                                client_frame_states[client_index].buffer,
                                client_frame_states[client_index].total_size,
                                client_frame_states[client_index].width,
                                client_frame_states[client_index].height,
                                client_frame_states[client_index].face_x,
                                client_frame_states[client_index].face_y,
                                client_frame_states[client_index].face_w,
                                client_frame_states[client_index].face_h,
                                client_frame_states[client_index].keypoints
                            );

                            const char* ack_msg = "{\"type\":\"frame_ack\"}";
                            websocket_server_send_text_client(httpd_req_to_sockfd(req), ack_msg);
                        }
                        else {
                            ESP_LOGE(TAG, "Frame end for ID %u received, but size mismatch! Expected %d, got %d",
                                (unsigned int)client_frame_states[client_index].id,
                                (int)client_frame_states[client_index].total_size,
                                (int)client_frame_states[client_index].received_size);
                        }
                    }
                    else {
                        ESP_LOGW(TAG, "Received frame_end for fd %d but no frame was being received.", httpd_req_to_sockfd(req));
                    }
                    reset_client_frame_state(httpd_req_to_sockfd(req)); // Always reset state after frame_end
                }
                else { // Unknown text message type
                    ESP_LOGW(TAG, "Received unknown text message type: %s from fd %d", type->valuestring, httpd_req_to_sockfd(req));
                }
            }
            else { // 'type' field is not a string
                ESP_LOGW(TAG, "Received text message with missing or non-string 'type' field from fd %d. Raw: %s", httpd_req_to_sockfd(req), (const char*)buf);
            }
            cJSON_Delete(root);
            free(buf);
        }
        else { // Empty text frame
            ESP_LOGW(TAG, "Received empty text frame from fd %d", httpd_req_to_sockfd(req));
        }
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        if (client_frame_states[client_index].is_receiving) {
            if (ws_pkt.len > 0 && (client_frame_states[client_index].received_size + ws_pkt.len <= client_frame_states[client_index].total_size)) {
                // Read the binary payload into the pre-allocated buffer
                ws_pkt.payload = client_frame_states[client_index].buffer + client_frame_states[client_index].received_size;
                ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
                if (ret == ESP_OK) {
                    client_frame_states[client_index].received_size += ws_pkt.len;
                }
                else {
                    ESP_LOGE(TAG, "httpd_ws_recv_frame (binary) error %d: %s for fd %d", ret, esp_err_to_name(ret), httpd_req_to_sockfd(req));
                    reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset on binary frame error
                }
            }
            else if (ws_pkt.len > 0 && (client_frame_states[client_index].received_size + ws_pkt.len > client_frame_states[client_index].total_size)) {
                ESP_LOGE(TAG, "Received binary data exceeds total_size for fd %d! Expected %d, current %d, received %d. Resetting state.",
                    httpd_req_to_sockfd(req), (int)client_frame_states[client_index].total_size,
                    (int)client_frame_states[client_index].received_size, (int)ws_pkt.len);
                uint8_t* dummy_buf = (uint8_t*)malloc(ws_pkt.len);
                if (dummy_buf) {
                    ws_pkt.payload = dummy_buf;
                    httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len); // Consume the data
                    free(dummy_buf);
                }
                else {
                    ESP_LOGE(TAG, "Failed to allocate dummy buffer to consume oversized data.");
                }
                reset_client_frame_state(httpd_req_to_sockfd(req)); // Reset on oversized data
            }
            else { // ws_pkt.len is 0 or negative
                ESP_LOGW(TAG, "Received zero-length binary data from fd %d while expecting data.", httpd_req_to_sockfd(req));
            }
        }
        else {
            ESP_LOGW(TAG, "Received unexpected binary data from fd %d (not in receiving state). Len: %zu", httpd_req_to_sockfd(req), ws_pkt.len);
            if (ws_pkt.len > 0) {
                uint8_t* dummy_buf = (uint8_t*)malloc(ws_pkt.len);
                if (dummy_buf) {
                    ws_pkt.payload = dummy_buf;
                    httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len); // Consume the data
                    free(dummy_buf);
                }
                else {
                    ESP_LOGE(TAG, "Failed to allocate dummy buffer to consume unexpected data.");
                }
            }
        }
    }
    else { // Other WebSocket frame types (Ping, Pong, Close). Can be extended...
        ESP_LOGD(TAG, "Received WebSocket frame type %d from fd %d", ws_pkt.type, httpd_req_to_sockfd(req));
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            reset_client_frame_state(httpd_req_to_sockfd(req));
        }
    }

    return ESP_OK;
}

// Connect/Initialize image processor
esp_err_t start_websocket_server(void) {
    if (server_handle != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    image_processor_init(); // Initialize image processor

    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        ws_clients[i].active = false;
        ws_clients[i].fd = -1;
        memset(&client_frame_states[i], 0, sizeof(frame_receive_state_t));
        client_frame_states[i].face_x = 0;
        client_frame_states[i].face_y = 0;
        client_frame_states[i].face_w = 0;
        client_frame_states[i].face_h = 0;
        client_frame_states[i].keypoints.clear(); // Initialize keypoints vector
    }

    config.server_port = WEBSOCKET_PORT;
    config.lru_purge_enable = true;
    config.stack_size = 24576; // can further experiment with optimal stack size
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;

    ESP_LOGD(TAG, "Websocket server on, port: '%d', stack size %d, recv_timeout %d, send_timeout %d",
        config.server_port, config.stack_size, config.recv_wait_timeout, config.send_wait_timeout);

    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting Websocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(server_handle, &ws_uri);
    if (ret != ESP_OK) {
        httpd_stop(server_handle);
        server_handle = NULL;
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTP_SERVER_EVENT,
        HTTP_SERVER_EVENT_DISCONNECTED,
        server_event_handler,
        server_handle));
    ESP_LOGD(TAG, "WebSocket server up & running!");
    return ESP_OK;
}

esp_err_t stop_websocket_server(void) {
    if (server_handle) {
        esp_event_handler_unregister(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_DISCONNECTED, server_event_handler);
        httpd_stop(server_handle);
        server_handle = NULL;
    }
    return ESP_OK;
}

typedef struct {
    int fd;
    char* data;
} async_send_arg_t;

esp_err_t websocket_server_send_text_client(int fd, const char* data) {
    if (!server_handle) return ESP_FAIL;
    if (fd < 0) return ESP_ERR_INVALID_ARG;

    async_send_arg_t* task_arg = (async_send_arg_t*)malloc(sizeof(async_send_arg_t));
    if (!task_arg) {
        ESP_LOGE(TAG, "Failed to allocate memory for async_send_arg_t.");
        return ESP_ERR_NO_MEM;
    }

    task_arg->fd = fd;
    task_arg->data = strdup(data);
    if (!task_arg->data) {
        ESP_LOGE(TAG, "Failed to duplicate string for async send.");
        free(task_arg);
        return ESP_ERR_NO_MEM;
    }

    if (httpd_queue_work(server_handle, ws_async_send, task_arg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue async send work for fd %d.", fd);
        free(task_arg->data);
        free(task_arg);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t websocket_server_send_text_all(const char* data) {
    if (!server_handle) return ESP_FAIL;

    int active_clients_count = 0;
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        if (ws_clients[i].active) {
            active_clients_count++;
            websocket_server_send_text_client(ws_clients[i].fd, data);
        }
    }

    if (active_clients_count == 0) {
        ESP_LOGW(TAG, "websocket_server_send_text_all: No active clients found to send to!");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void ws_async_send(void* arg) {
    async_send_arg_t* send_arg = (async_send_arg_t*)arg;
    int fd = send_arg->fd;
    char* data_to_send = send_arg->data;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data_to_send;
    ws_pkt.len = strlen(data_to_send);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.final = true;

    esp_err_t err = httpd_ws_send_frame_async(server_handle, fd, &ws_pkt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send async WebSocket frame to fd %d: %s", fd, esp_err_to_name(err));
    }

    free(data_to_send);
    free(send_arg);
}

bool websocket_server_is_client_connected(void) {
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        if (ws_clients[i].active) {
            return true;
        }
    }
    return false;
}