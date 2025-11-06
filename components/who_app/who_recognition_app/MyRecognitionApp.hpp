#pragma once
#include "who_recognition_app_lcd.hpp"
#include "who_recognition.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

extern "C" bool http_streaming_active(void);

extern "C" bool send_rgb565_image_to_telegram(const uint8_t *rgb565,
                                               uint16_t width,
                                               uint16_t height,
                                               uint8_t quality,
                                               const char *caption);
extern "C" bool send_json_over_tcp(const char *ip, uint16_t port, const char *json, size_t len);

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap),
          m_recognition_pending(false),
          m_detection_active(false),
          m_detection_start_tick(0),
          m_last_detection_tick(0)

    {

    }

    // Expose recognition event group for web control
    EventGroupHandle_t get_recognition_event_group() const {
        return m_recognition->get_recognition_task()->get_event_group();
    }

protected:
    // Recognition result from core
    void recognition_result_cb(const std::string &result) override {
        // Optional LCD overlay (enabled only when not streaming to avoid SPI/CPU contention)
        if (lcd_enabled()) {
            who::app::WhoRecognitionAppLCD::recognition_result_cb(result);
        }
        // allow next trigger
        m_recognition_pending = false;
        ESP_LOGI("Recognition", "%s", result.c_str());
        if (result.find("id: ") != std::string::npos) {
            ESP_LOGI("Recognition", "Recognized face, resetting detection state.");
            m_detection_active = false;
            m_detection_start_tick = 0;
            int id = -1; float sim = 0.0f;
            if (std::sscanf(result.c_str(), "id: %d, sim: %f", &id, &sim) == 2) {
                char json[128];
                int n = std::snprintf(json, sizeof(json),
                                      "{\"event\":\"recognition\",\"id\":%d,\"sim\":%.2f}", id, sim);
                if (n > 0) {
                    (void)send_json_over_tcp(ESP32_Receiver_IP, ESP32_Receiver_Port, json, (size_t)n);
                }
            }
        }
        else if (result.find("who?") != std::string::npos) {
            // Unknown face: send JSON too if desired
            const char *json = "{\"event\":\"recognition\",\"id\":null,\"sim\":null}";
            (void)send_json_over_tcp(ESP32_Receiver_IP, ESP32_Receiver_Port, json, strlen(json));
        }
    }

    // Detection results (bounding boxes, etc.)
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override {
        // Optional LCD overlay (enabled only when not streaming to avoid SPI/CPU contention)
        if (lcd_enabled()) {
            who::app::WhoRecognitionAppLCD::detect_result_cb(result);
        }
        const TickType_t one_sec = pdMS_TO_TICKS(1000);
        const TickType_t min_send_interval = pdMS_TO_TICKS(10000); // cooldown between motion snapshots
        TickType_t now = xTaskGetTickCount();
        if (!result.det_res.empty()) {
            if (!m_detection_active) {
                m_detection_active = true;
                m_detection_start_tick = now;
            }
            m_last_detection_tick = now;
            // If detection sustained for >= 1s, trigger recognition first
            if (!m_recognition_pending && (now - m_detection_start_tick >= one_sec)) {
                auto *rec_task = m_recognition->get_recognition_task();
                if (rec_task && rec_task->is_active()) {
                    xEventGroupSetBits(rec_task->get_event_group(), who::recognition::WhoRecognitionCore::RECOGNIZE);
                    m_recognition_pending = true;
                }
            }
            // Block snapshots while recognition is pending OR while detection is sustained (>=1s)
            const bool recognition_window = m_recognition_pending || ((now - m_detection_start_tick) >= one_sec);
            // Only consider snapshot outside recognition window and when cooldown elapsed
            if (!recognition_window && (now - m_last_send_tick >= min_send_interval)) {
                auto last_node = m_frame_cap->get_last_node();
                if (last_node) {
                    who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                    if (fb && fb->buf && fb->width && fb->height) {
                        ESP_LOGI("Detection", "Motion snapshot: queue send task");
                        const size_t rgb_len = (size_t)fb->width * fb->height * 2;
                        uint8_t *copy = (uint8_t *)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!copy) {
                            copy = (uint8_t *)malloc(rgb_len);
                        }
                        if (copy) {
                            memcpy(copy, fb->buf, rgb_len);
                            // Prepare context
                            auto *ctx = (MotionSendCtx *)malloc(sizeof(MotionSendCtx));
                            if (ctx) {
                                ctx->rgb565 = copy;
                                ctx->len = rgb_len;
                                ctx->width = fb->width;
                                ctx->height = fb->height;
                                ctx->quality = 40;
                                strncpy(ctx->caption, "Motion Detected", sizeof(ctx->caption) - 1);
                                ctx->caption[sizeof(ctx->caption) - 1] = '\0';
                                // TLS handshake can consume deep call stacks via esp_http_client + mbedTLS.
                                // Use a larger stack to avoid overflows observed on some networks.
                                const uint32_t stack_words = 8192; // words (x4 bytes) => ~32KB
                                BaseType_t ok = xTaskCreatePinnedToCore(&MyRecognitionApp::MotionSendTask,
                                                                        "MotionSend",
                                                                        stack_words,
                                                                        (void *)ctx,
                                                                        tskIDLE_PRIORITY, // keep lowest priority so detection/recognition win
                                                                        nullptr,
                                                                        0);
                                if (ok != pdPASS) {
                                    ESP_LOGE("Detection", "Failed to create MotionSend task");
                                    free(copy);
                                    free(ctx);
                                } else {
                                    m_last_send_tick = now;
                                }
                            } else {
                                ESP_LOGE("Detection", "Failed to alloc MotionSendCtx");
                                free(copy);
                            }
                        } else {
                            ESP_LOGE("Detection", "Failed to alloc RGB565 copy (%ux%u)", fb->width, fb->height);
                        }
                    }
                }
            }
            // (recognition gating handled above)
        } else {
            // No detections in this frame; if it stays quiet for a short while, reset state
            const TickType_t quiet_ms = pdMS_TO_TICKS(300);
            if (m_detection_active && (now - m_last_detection_tick > quiet_ms)) {
                m_detection_active = false;
                m_detection_start_tick = 0;
            }
        }
    }

    void recognition_cleanup() override {
        who::app::WhoRecognitionAppLCD::recognition_cleanup();
        m_recognition_pending = false;
    }

    void detect_cleanup() override {
        who::app::WhoRecognitionAppLCD::detect_cleanup();
    }

private:
    // Decide dynamically if LCD overlays should be enabled
    static inline bool lcd_enabled() {
        return !http_streaming_active();
    }
    // Context and task to send snapshot without using Detect task stack
    struct MotionSendCtx {
        uint8_t *rgb565;
        size_t len;
        uint16_t width;
        uint16_t height;
        uint8_t quality;
        char caption[64];
    };

    static void MotionSendTask(void *arg) {
        MotionSendCtx *ctx = (MotionSendCtx *)arg;
        if (ctx) {
            // Yield once so critical tasks can run
            vTaskDelay(1);
            ESP_LOGI("MotionSend", "Sending snapshot to Telegram (%ux%u)", ctx->width, ctx->height);
            (void)send_rgb565_image_to_telegram(ctx->rgb565, ctx->width, ctx->height, ctx->quality, ctx->caption);
            free(ctx->rgb565);
            free(ctx);
        }
        vTaskDelete(nullptr);
    }

    // LCD drawing is dynamically gated via lcd_enabled()
    bool m_recognition_pending;
    bool m_detection_active;
    TickType_t m_detection_start_tick;
    TickType_t m_last_detection_tick;
    TickType_t m_motion_detected_tick;
    TickType_t m_last_send_tick {0};
};