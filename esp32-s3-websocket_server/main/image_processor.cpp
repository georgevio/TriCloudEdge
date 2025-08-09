/**
 * @file image_processor.cpp
 * @version 33
 * @brief Main logic for handling incoming images, recognition, and enrollment.
 */
#include "esp_log.h"
#include "image_processor.h"
#include "face_recognizer.hpp"
#include "face_database.h"
#include "storage_manager.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstring>
#include "esp_heap_caps.h" 
#include "esp_timer.h"
#include "s3_uploader.h"
#include "utils/mqtt.h"
#include "cJSON.h"
#include "utils/config.h" // Includes secret.h
#include "websocket_server.h"

#if ENABLE_ENROLLMENT 
#include "face_enroller.h" // For enroll_new_face function
#define ERASE_DATABASE_ON_STARTUP 1 // Set to 1 to clear database on startup, 0 to preserve it
#endif

static const char* TAG = "IMAGE_PROCESSOR";

// Forward declaration; includes width and height 
static void handle_unknown_face(uint8_t* image_buffer, size_t image_len, int width, int height);

// Constructor called once only, otherwise catastrophe hits (restes, etc.).
static FaceRecognizer s_face_recognizer;

/**
 * @brief Initializes the image processor module.
 * @return ESP_OK if successful, error code otherwise.
 */
esp_err_t image_processor_init(void) {
    ESP_LOGD(TAG, "Image handler init.");

#if ERASE_DATABASE_ON_STARTUP // BE CAREFUL: ERASES ALL DB ENTRIES!    
    ESP_LOGI(TAG, "Initiating database cleanup on startup to prepare for new enrollments.");
    if (database_clear_all() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear existing database entries. This might lead to inconsistent data.");
    }
    else {
        ESP_LOGI(TAG, "Database cleanup completed successfully. Starting with empty database for enrollment.");
    }
#endif

    ESP_LOGD(TAG, "Opening face metadata database for initial load.");
    // Load the face metadata database. Reads records from SPIFFS.
    if (database_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize database!");
        return ESP_FAIL; // Return failure if database init fails
    }
    ESP_LOGD(TAG, "Face metadata database opened successfully for initial load.");
    // De-initialize database after initial setup. It will be re-initialized in handle_new_image if needed.
    database_deinit();
    ESP_LOGD(TAG, "Image processor init complete. Database deinitialized after startup load.");
    return ESP_OK;
}

/**
 * @brief Workes upon an incoming (face detected) image for face recognition.
 *
 * Receives a cropped image buffer and metadata (face bounding box, keypoints) from
 * the client. It adjusts the coordinates for the esp-who AI model and
 * extracts an embedding. The cropped image has some buffer-zone around it (20px).
 *
 * @param image_buffer Pointer to the received image data (RGB565 format, already cropped).
 * @param image_len Length of the image_buffer in Bytes.
 * @param cropped_img_width Width of the cropped face image_buffer
 * @param cropped_img_height Height of the cropped face image_buffer
 * @param original_face_x X-coordinate of the bounding box relative to the *original full camera frame*.
 * @param original_face_y Y-coordinate of the bounding box relative to the *original full camera frame*.
 * @param face_w Width of the face bounding box (same as cropped_img_width).
 * @param face_h Height of the face bounding box (same as cropped_img_height).
 * @param keypoints A vector of integers for facial keypoints, relative to the *original full camera frame*.
 * @return ESP_OK if processing is successful, error code otherwise.
 */
esp_err_t image_processor_handle_new_image(
    uint8_t* image_buffer, size_t image_len, int cropped_img_width, int cropped_img_height,
    int original_face_x, int original_face_y, int face_w, int face_h,
    const std::vector<int>& keypoints) {

    // Log the initial received data for verification
    ESP_LOGD(TAG, "New image Buffer Address: %p", image_buffer);
    ESP_LOGD(TAG, "  Buffer Length (bytes): %zu", image_len);
    ESP_LOGD(TAG, "  Cropped Image Width x Height: %d x %d", cropped_img_width, cropped_img_height);
    ESP_LOGD(TAG, "  Original Face Box (relative to full frame) - X:%d, Y:%d, W:%d, H:%d",
        original_face_x, original_face_y, face_w, face_h);
    ESP_LOGD(TAG, "  Keypoints received (count: %zu):", keypoints.size());

    char kp_buf[200]; // Buffer for keypoints string
    int offset = snprintf(kp_buf, sizeof(kp_buf) - 1, "[");
    for (size_t i = 0; i < keypoints.size(); ++i) {
        offset += snprintf(kp_buf + offset, sizeof(kp_buf) - offset - 1, "%d", keypoints[i]);
        if (i < keypoints.size() - 1) {
            offset += snprintf(kp_buf + offset, sizeof(kp_buf) - offset - 1, ",");
        }
    }

    snprintf(kp_buf + offset, sizeof(kp_buf) - offset, "]");
    ESP_LOGD(TAG, "    %s", kp_buf);
    ESP_LOGD(TAG, " ---- End Received Data Log for Incoming Image ---");
    ESP_LOGD(TAG, "Starting AI model feature extraction for incoming image.");

    // Adjust face box and keypoints to the cropped image, not to the original frame.
    int adjusted_face_x = 0;
    int adjusted_face_y = 0;

    std::vector<int> adjusted_keypoints = keypoints;
    for (size_t i = 0; i < adjusted_keypoints.size(); i += 2) {
        adjusted_keypoints[i] -= original_face_x;
        adjusted_keypoints[i + 1] -= original_face_y;
    }
    ESP_LOGD(TAG, "Adjusted Face Box for FaceRecognizer: X:%d, Y:%d, W:%d, H:%d",
        adjusted_face_x, adjusted_face_y, face_w, face_h);
    ESP_LOGD(TAG, "Adjusted Keypoints (count: %zu):", adjusted_keypoints.size());

    offset = snprintf(kp_buf, sizeof(kp_buf) - 1, "[");
    for (size_t i = 0; i < adjusted_keypoints.size(); ++i) {
        offset += snprintf(kp_buf + offset, sizeof(kp_buf) - offset - 1, "%d", adjusted_keypoints[i]);
        if (i < keypoints.size() - 1) {
            offset += snprintf(kp_buf + offset, sizeof(kp_buf) - offset - 1, ",");
        }
    }
    snprintf(kp_buf + offset, sizeof(kp_buf) - offset, "]");
    ESP_LOGD(TAG, "    %s", kp_buf);

    // Use the global/static s_face_recognizer instance to extract the embedding.
    std::vector<float>* incoming_embedding = s_face_recognizer.extract_embedding_from_cropped_box(
        image_buffer, cropped_img_width, cropped_img_height,
        adjusted_face_x, adjusted_face_y, face_w, face_h,
        adjusted_keypoints
    );

    if (!incoming_embedding) {
        ESP_LOGE(TAG, "Incoming image features extraction error...");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Successfully extracted embedding from incoming image (size: %zu).", incoming_embedding->size());

    // Database Comparison Loop
    face_record_t* db_faces_ptr = NULL;
    int db_face_count = 0;
    int recognized_id = -1;
    const char* recognized_name = "Unknown";
    float max_similarity = 0.0f;
    ESP_LOGD(TAG, "Starting DB comparison for incoming image.");

    // Re-initialize the fresh database
    if (database_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-initialize database.");
        delete incoming_embedding;
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Database re-initialized for comparison loop.");

    if (database_get_all_faces(&db_faces_ptr, &db_face_count) == ESP_OK) {
        if (db_face_count == 0) {
            ESP_LOGW(TAG, "Empty dB!");
        }
        else {
            for (int i = 0; i < db_face_count; i++) {
                const face_record_t* current_face = &db_faces_ptr[i];
                ESP_LOGD(TAG, "--- Comparing with dB face #%d (ID: %d, Name: %s) from file: %s ---",
                    i, current_face->id, current_face->name, current_face->embedding_file);

                char* stored_embedding_bytes_raw = NULL;
                size_t stored_embedding_len = 0;

                esp_err_t read_err = storage_read_file(current_face->embedding_file, &stored_embedding_bytes_raw, &stored_embedding_len);

                if (read_err == ESP_OK && stored_embedding_bytes_raw != NULL) {
                    ESP_LOGD(TAG, "Stored embedding for %s, size: %zu bytes.", current_face->name, stored_embedding_len);
                    size_t expected_embedding_dim = incoming_embedding->size();

                    if (stored_embedding_len != (expected_embedding_dim * sizeof(float))) {
                        ESP_LOGE(TAG, "Embedding file %s has unexpected size (actual: %zu bytes, expected: %zu bytes). Skipping comparison.",
                            current_face->embedding_file, stored_embedding_len, expected_embedding_dim * sizeof(float));
                        free(stored_embedding_bytes_raw);
                        continue;
                    }

                    std::vector<float> stored_embedding(expected_embedding_dim);
                    memcpy(stored_embedding.data(), stored_embedding_bytes_raw, expected_embedding_dim * sizeof(float));
                    free(stored_embedding_bytes_raw);

                    float similarity = s_face_recognizer.compare_embeddings(*incoming_embedding, stored_embedding);
                    ESP_LOGD(TAG, "Cosine similarity with %s (ID %d): %f", current_face->name, current_face->id, similarity);

                    if (similarity > max_similarity) {
                        max_similarity = similarity;
                        recognized_id = current_face->id;
                        recognized_name = current_face->name;
                        ESP_LOGI(TAG, "%s,  similarity: %f", recognized_name, max_similarity);
                        ESP_LOGD(TAG, "DB Entry %d: ", recognized_id);
                    }
                }
                else {
                    ESP_LOGE(TAG, "Failed to read embedding file %s. Error: %s.", current_face->embedding_file, esp_err_to_name(read_err));
                    if (stored_embedding_bytes_raw) free(stored_embedding_bytes_raw);
                }
            }
        }
    }
    else {
        ESP_LOGE(TAG, "Failed to get dB data.");
    }
    ESP_LOGD(TAG, "Comparison completed. Best similarity found: %f", max_similarity);

    // Final decision: compares with similarity threshold in config.h
    if (recognized_id >= 0 && max_similarity >= COSINE_SIMILARITY_THRESHOLD) {
        ESP_LOGI(TAG, "\033[1;32m******************************************\033[0m");
        ESP_LOGI(TAG, "\033[1;32m FACE RECOGNIZED! ID: %d (%s) \033[0m", recognized_id, recognized_name);
        ESP_LOGI(TAG, "\033[1;32m******************************************\033[0m");
    
        // send back to websocket client(s) the recognized face details
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "type", "recognition_result");
            cJSON_AddStringToObject(root, "name", recognized_name);
            cJSON_AddStringToObject(root, "source", "local");

            char *json_payload = cJSON_PrintUnformatted(root);
            if (json_payload) {
                ESP_LOGD(TAG, "Sending payload: %s", json_payload);
                websocket_server_send_text_all(json_payload);
                free(json_payload);
            }
            cJSON_Delete(root);
        }
    }
    else {
        ESP_LOGI(TAG, "\033[1;36m******************************************\033[0m");
        ESP_LOGI(TAG, "\033[1;36m UNKNOWN FACE (Best Similarity: %f) \033[0m", max_similarity);
        ESP_LOGI(TAG, "\033[1;36m******************************************\033[0m");
    
#if SEND_UNKNOWN_FACES_TO_AWS
        handle_unknown_face(image_buffer, image_len, cropped_img_width, cropped_img_height);
#endif
    }

    ESP_LOGD(TAG, "De-initializing database after comparison loop.");
    database_deinit();
    ESP_LOGD(TAG, "Database de-initialized.");

#if ENABLE_ENROLLMENT 
    ESP_LOGI(TAG, "Enrollment is ENABLED. Proceeding to enroll new incoming face.");
    esp_err_t enroll_res = enroll_new_face(
        image_buffer, image_len, cropped_img_width, cropped_img_height,
        adjusted_face_x, adjusted_face_y, face_w, face_h,
        adjusted_keypoints
    );
    if (enroll_res == ESP_OK) {
        ESP_LOGI(TAG, "New face enrollment process for incoming image initiated successfully.");
    }
    else {
        ESP_LOGE(TAG, "Failed to enroll new incoming face. Error: %s", esp_err_to_name(enroll_res));
    }
#endif

    delete incoming_embedding;
    ESP_LOGD(TAG, "Image processing complete. Return from image_processor_handle_new_image");
    return ESP_OK;
}

/**
 * @brief Placeholder function to "process" an image embedding loaded from the database.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t image_processor_handle_database_image(
    const uint8_t* embedding_data, size_t embedding_len, int record_id, const char* record_name) {
    ESP_LOGD(TAG, "--- Processing Database Image ---");
    ESP_LOGD(TAG, "  Database Image: ID %d, Name '%s', Embedding Length: %zu bytes", record_id, record_name, embedding_len);
    ESP_LOGD(TAG, "--- Finished Processing Database Image ---");
    return ESP_OK;
}

/* If the face is unknown locally, sent to AWS for further analysis */
static void handle_unknown_face(uint8_t* image_buffer, size_t image_len, int width, int height) {
    ESP_LOGI(TAG, "Uploading unrecognized face to S3.");

    char filename[64];
    snprintf(filename, sizeof(filename), "%dx%d_%lld.bin", width, height, esp_timer_get_time());

    char presigned_url[2048];
    esp_err_t url_err = s3_uploader_get_presigned_url(filename, presigned_url, sizeof(presigned_url));

    if (url_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get pre-signed URL for S3 upload.");
        return;
    }

    esp_err_t upload_err = s3_uploader_upload_by_url(
        presigned_url, image_buffer, image_len, "application/octet-stream");

    if (upload_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to upload image to S3.");
        return;
    }

    ESP_LOGI(TAG, "Image %s uloaded to S3.", filename);

    // After succesful upload, publish an MQTT message
    if (mqtt_is_connected()) { // check again if MQTT is connected
        esp_mqtt_client_handle_t mqtt_client = get_mqtt_client();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event", "unknown_face_detected");
        cJSON_AddStringToObject(root, "s3_key", filename);
        cJSON_AddStringToObject(root, "device_id", AWS_IOT_CLIENT_ID);

        char* json_payload = cJSON_PrintUnformatted(root);
        if (json_payload) {
            mqtt_publish_message(mqtt_client, "faces/unknown", json_payload, 1, 0);
            free(json_payload);
        }
        else {
            ESP_LOGE(TAG, "Failed to print JSON payload for MQTT.");
        }
        cJSON_Delete(root);
    }
    else {
        ESP_LOGE(TAG, "MQTT not connected, cannot publish notification for unknown face.");
    }
}
