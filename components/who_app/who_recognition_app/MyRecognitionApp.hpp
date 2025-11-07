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
extern "C" void http_motion_set_state(bool active);

// Async network sender enqueue APIs
extern "C" bool net_send_http_plain_async(const char *ip, uint16_t port, const char *path, const char *body, size_t len);
extern "C" bool net_send_telegram_rgb565_take(uint8_t *rgb565, size_t rgb565_len, uint16_t width, uint16_t height, uint8_t quality, const char *caption);

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
    void recognition_result_cb(const std::string &result) override {
        // disable LCD if needed
        if (lcd_enabled()) {
            who::app::WhoRecognitionAppLCD::recognition_result_cb(result);
        }
        m_recognition_pending = false;
        ESP_LOGI("Recognition", "%s", result.c_str());
        const TickType_t one_sec = pdMS_TO_TICKS(1000);
        TickType_t now_tick = xTaskGetTickCount();

        // Parse results for id and sim 
        bool cur_known = false;
        int cur_id = -1;
        float cur_sim = 0.0f;
        if (result.find("id: ") != std::string::npos) {
            int id = -1; float sim = 0.0f;
            if (std::sscanf(result.c_str(), "id: %d, sim: %f", &id, &sim) == 2) {
                cur_known = true;
                cur_id = id;
                cur_sim = sim;
            } else {
                return; // ignore other messages
            }
        } else if (result.find("who?") != std::string::npos) {
            cur_known = false;
            cur_id = -1;
            cur_sim = 0.0f;
        } else {
            return; // ignore other messages
        }

        // Reset detection 
        m_detection_active = false;
        m_detection_start_tick = 0;

        // Require same outcome for 1s before posting
        if (m_stable_first) {
            m_stable_first = false;
            m_stable_known = cur_known;
            m_stable_id = cur_id;
            m_stable_sim = cur_sim;
            m_stable_start_tick = now_tick;
            m_stable_sent = false;
            m_last_stable_post_tick = 0;
            return;
        }

        // Different outcome: reset stability timer
        if (m_stable_known != cur_known || m_stable_id != cur_id) {
            m_stable_known = cur_known;
            m_stable_id = cur_id;
            m_stable_sim = cur_sim;
            m_stable_start_tick = now_tick;
            m_stable_sent = false;
            m_last_stable_post_tick = 0;
            return;
        }

        // Same outcome: send post
        m_stable_sim = cur_sim;
        if (now_tick - m_stable_start_tick >= one_sec) {
            bool should_send = false;
            if (!m_stable_sent) {
                should_send = true; // first send after stable
            } else if (now_tick - m_last_stable_post_tick >= pdMS_TO_TICKS(10000)) {
                should_send = true; // periodic resend every 10s while stable
            }
            if (should_send) {
                if (m_stable_known) {
                    char body[64];
                    int n = std::snprintf(body, sizeof(body), "authorized,%.2f", m_stable_sim);
                    if (n > 0) {
                        if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
                        net_send_http_plain_async(ESP32_Receiver_IP, ESP32_Receiver_Port, ESP32_Receiver_Path, body, (size_t)n);
                    }
                } else {
                    const char *body = "denied,0";
                    net_send_http_plain_async(ESP32_Receiver_IP, ESP32_Receiver_Port, ESP32_Receiver_Path, body, strlen(body));
                }
                m_stable_sent = true;
                m_last_stable_post_tick = now_tick;
            }
        }
    }

    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override {
        // Draw to LCD if enabled
        if (lcd_enabled()) {
            who::app::WhoRecognitionAppLCD::detect_result_cb(result);
        }

        const TickType_t now = xTaskGetTickCount();
        const TickType_t snapshot_cooldown = pdMS_TO_TICKS(30000); // snapshot every 30s max

        if (!result.det_res.empty()) {
            http_motion_set_state(true);
            // Send snapshot if cooldown elapsed
            if (now - m_last_send_tick >= snapshot_cooldown) {
                auto last_node = m_frame_cap->get_last_node();
                if (last_node) {
                    who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                    if (fb && fb->buf && fb->width && fb->height) {
                        const size_t rgb_len = (size_t)fb->width * fb->height * 2;
                        uint8_t *copy = (uint8_t *)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!copy) copy = (uint8_t *)malloc(rgb_len);
                        if (copy) {
                            memcpy(copy, fb->buf, rgb_len);
                            char caption[64];
                            strncpy(caption, "Motion Detected", sizeof(caption) - 1);
                            caption[sizeof(caption) - 1] = '\0';
                            if (net_send_telegram_rgb565_take(copy, rgb_len, fb->width, fb->height, 40, caption)) {
                                m_last_send_tick = now;
                            } else {
                                // Fallback: free copy (since queue didn't take ownership)
                                free(copy);
                            }
                        } else {
                            ESP_LOGE("Detection", "Alloc snapshot buffer failed (%ux%u)", fb->width, fb->height);
                        }
                    }
                }
            }
        } else {
            http_motion_set_state(false);
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
    // LCD enabled
    static inline bool lcd_enabled() {
        return true;
        //return !http_streaming_active() && !s_sending_snapshot;
    }

    struct MotionSendCtx {
        uint8_t *rgb565;
        size_t len;
        uint16_t width;
        uint16_t height;
        uint8_t quality;
        char caption[64];
    };

    // send snapshot over tcp when motion detected
    static void MotionSendTask(void *arg) {
        MotionSendCtx *ctx = (MotionSendCtx *)arg;
        if (ctx) {
            vTaskDelay(100);
            s_sending_snapshot = true;
            UBaseType_t hw_before = uxTaskGetStackHighWaterMark(nullptr);
            ESP_LOGI("MotionSend", "Sending snapshot to Telegram (%ux%u)", ctx->width, ctx->height);
            (void)send_rgb565_image_to_telegram(ctx->rgb565, ctx->width, ctx->height, ctx->quality, ctx->caption);
            UBaseType_t hw_after = uxTaskGetStackHighWaterMark(nullptr);
            ESP_LOGI("MotionSend", "Stack watermark (words) before/after: %u/%u", (unsigned)hw_before, (unsigned)hw_after);
            s_sending_snapshot = false;
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
    static inline volatile bool s_sending_snapshot = false;
    // Recognition stability gating
    bool m_stable_first {true};
    bool m_stable_known {false};
    int m_stable_id {-1};
    float m_stable_sim {0.0f};
    TickType_t m_stable_start_tick {0};
    bool m_stable_sent {false};
    TickType_t m_last_stable_post_tick {0};
};