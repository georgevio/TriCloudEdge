#include "face_sender.h"
#include "who_camera.h"
#include "who_human_face_detection.hpp"
#include "websocket_client.h"
#include "esp_log.h"
#include "utils/config.h"
#include <vector>
#include <inttypes.h>
#include <algorithm>
#include <string.h>

static EventGroupHandle_t s_app_event_group;
static QueueHandle_t xQueueAIFrame;
static QueueHandle_t xQueueFaceFrame;

static const char* TAG = "FACE_SENDER";

/**
 * @brief Converts a std::vector of integers to a JSON array formatted string.
 * @param vec The input vector of integers.
 * @param buffer The character buffer to store JSON string results.
 * @param buffer_len Output buffer size.
 * Formats the vector as a comma-separated list within square brackets for JSON compatibility.
 */
static void int_vector_to_json_string(const std::vector<int>& vec, 
        char* buffer, size_t buffer_len) {
    size_t offset = 0;
    offset += snprintf(buffer + offset, buffer_len - offset, "[");
    for (size_t i = 0; i < vec.size(); ++i) {
        offset += snprintf(buffer + offset, buffer_len - offset, "%d", vec[i]);
        if (i < vec.size() - 1) {
            offset += snprintf(buffer + offset, buffer_len - offset, ",");
        }
    }
    snprintf(buffer + offset, buffer_len - offset, "]");
}

/**
 * @brief Initialize and provide the necessary handles to the face sender module.
 * @param app_event_group Handle to the main application event group.
 * @param ai_queue Handle to the queue for incoming AI frames.
 * @param face_queue Handle to the queue for detected faces to be sent.
 */
void face_sender_init(EventGroupHandle_t app_event_group, 
        QueueHandle_t ai_queue, QueueHandle_t face_queue) {
    s_app_event_group = app_event_group;
    xQueueAIFrame = ai_queue;
    xQueueFaceFrame = face_queue;
}

/**
 * @brief Process and send detected face data over WebSocket.
 * @param pvParameters Unused task parameters.
 * Waits for face data on the xQueueFaceFrame queue. When a face is received,
 * it stops the camera, crops the face from the full frame with a margin 
 * (can be adapted), adjusts keypoint coordinates, and transmits the cropped 
 * image and metadata in chunks over the WebSocket. 
 * After a successful transfer and server acknowledgment, t enters a cooldown 
 * period before restarting the camera to avoid multiple same face detections.
 */
void face_sending_task(void* pvParameters) {
    face_to_send_t *face_data = NULL;
    const size_t CHUNK_SIZE = WEBSOCKET_CHUNK_SIZE;
    char keypoints_json_str[150];
    char start_msg[350];

    while (true) {
        if (xQueueReceive(xQueueFaceFrame, &face_data, portMAX_DELAY)) {
            if (!face_data || !face_data->fb) {
                if (face_data) {
                    if (face_data->fb) esp_camera_fb_return(face_data->fb);
                    free(face_data);
                }
                continue;
            }

            camera_fb_t* full_frame = face_data->fb;
            ESP_LOGI(TAG, "\033[1;33m*************************************\033[0m");
            ESP_LOGI(TAG, "\033[1;32m       FACE DETECTED in frame %" PRIu32 "\033[0m", face_data->id);
            ESP_LOGI(TAG, "\033[1;33m*************************************\033[0m");
                        
            camera_stop(); //prevent duplicates of the same face

            uint8_t *cropped_buf = NULL;
            do {
                int original_face_x = face_data->box.x;
                int original_face_y = face_data->box.y;
                int original_face_w = face_data->box.w;
                int original_face_h = face_data->box.h;
                uint32_t frame_id = face_data->id;

                int crop_x_start = std::max(0, original_face_x - FACE_CROP_MARGIN_PIXELS);
                int crop_y_start = std::max(0, original_face_y - FACE_CROP_MARGIN_PIXELS);
                int crop_x_end = std::min((int)full_frame->width, original_face_x + original_face_w + FACE_CROP_MARGIN_PIXELS);
                int crop_y_end = std::min((int)full_frame->height, original_face_y + original_face_h + FACE_CROP_MARGIN_PIXELS);
                int cropped_img_width = crop_x_end - crop_x_start;
                int cropped_img_height = crop_y_end - crop_y_start;

                if (cropped_img_width <= 0 || cropped_img_height <= 0) {
                    ESP_LOGE(TAG, "Invalid crop dimensions for frame %" PRIu32, frame_id);
                    break;
                }

                std::vector<int> adjusted_keypoints = face_data->keypoint;
                for (size_t i = 0; i < adjusted_keypoints.size(); i += 2) {
                    adjusted_keypoints[i] -= crop_x_start;
                    adjusted_keypoints[i+1] -= crop_y_start;
                }
                int_vector_to_json_string(adjusted_keypoints, keypoints_json_str, sizeof(keypoints_json_str));

                size_t cropped_len = (size_t)cropped_img_width * (size_t)cropped_img_height * 2;
                cropped_buf = (uint8_t *)malloc(cropped_len);
                if (!cropped_buf) {
                    ESP_LOGE(TAG, "Failed to allocate memory for cropped frame: %zu Bytes", cropped_len);
                    break;
                }

                uint16_t *p_full = (uint16_t *)full_frame->buf;
                uint16_t *p_cropped = (uint16_t *)cropped_buf;
                for (int row = 0; row < cropped_img_height; ++row) {
                    memcpy(p_cropped + (row * cropped_img_width), p_full + ((crop_y_start + row) * full_frame->width) + crop_x_start, cropped_img_width * sizeof(uint16_t));
                }
                
                xEventGroupWaitBits(s_app_event_group, WIFI_CONNECTED_BIT | WEBSOCKET_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
                xEventGroupClearBits(s_app_event_group, FRAME_ACK_BIT);

                snprintf(start_msg, sizeof(start_msg), "{\"type\":\"frame_start\", \"size\":%zu, \"id\":%" PRIu32 ", \"width\":%d, \"height\":%d, \"box_x\":%d, \"box_y\":%d, \"box_w\":%d, \"box_h\":%d, \"keypoints\":%s}",
                         cropped_len, frame_id, cropped_img_width, cropped_img_height,
                         original_face_x, original_face_y, original_face_w, original_face_h,
                         keypoints_json_str);

                ESP_LOGI(TAG, "\033[1;33m↑↑↑ Sending frame %" PRIu32 " ↑↑↑\033[0m", frame_id);

                if(websocket_send_text(start_msg) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send frame_start. Aborting!");
                    break;
                }

                uint8_t *p_buffer = cropped_buf;
                size_t remaining = cropped_len;
                while (remaining > 0) {
                    size_t to_send = std::min(remaining, CHUNK_SIZE);
                    if (websocket_send_frame(p_buffer, to_send) != ESP_OK) {
                        ESP_LOGE(TAG, "Chunk send failed for frame %" PRIu32 ". Aborting.", frame_id);
                        remaining = 1; // Mark as failed
                        break;
                    }
                    p_buffer += to_send;
                    remaining -= to_send;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                if (remaining > 0) break; // Exit if transfer failed

                websocket_send_text("{\"type\":\"frame_end\"}");
                
                EventBits_t bits = xEventGroupWaitBits(s_app_event_group, FRAME_ACK_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(SERVER_ACK_TIMEOUT_MS));
                if (bits & FRAME_ACK_BIT) {
                    ESP_LOGI(TAG, "ACK received for frame %" PRIu32 "!", frame_id);
                } else {
                    ESP_LOGE(TAG, "ACK timeout for frame %" PRIu32, frame_id);
                }

            } while(0);

            // TODO: is this a memory leak?
            esp_camera_fb_return(full_frame);
            // if yes, this is safer
            // Correct way to free the manually allocated frame buffer
            //free(full_frame->buf);
            //free(full_frame);

            if (cropped_buf) free(cropped_buf);
            free(face_data);
            
            ESP_LOGI(TAG, "Entering %d sec cooldown.", POST_DETECTION_COOLDOWN_S);
            vTaskDelay(pdMS_TO_TICKS(POST_DETECTION_COOLDOWN_S * 1000));
            
            ESP_LOGD(TAG, "Cooldown ended. Flushing queues before restart.");
            xQueueReset(xQueueAIFrame);
            xQueueReset(xQueueFaceFrame);

            camera_start();
            ESP_LOGI(TAG, "Camera (re)started. Waiting to detect faces.");
        }
    }
}