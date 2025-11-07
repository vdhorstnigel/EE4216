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

// Async network sender enqueue APIs
extern "C" bool net_send_http_plain_async(const char *ip, uint16_t port, const char *path, const char *body, size_t len);

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap)

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

        ESP_LOGI("Recognition", "%s", result.c_str());
        const TickType_t one_sec = pdMS_TO_TICKS(1000);
        TickType_t now_tick = xTaskGetTickCount();

        // Parse results for id and similarity
        bool cur_known = false;
        int cur_id = -1;
        float cur_sim = 0.0f;

        if (result.find("id: ") != std::string::npos) {
            int id = -1; float parsed_sim = 0.0f;
            if (std::sscanf(result.c_str(), "id: %d, sim: %f", &id, &parsed_sim) == 2) {
                cur_known = true;
                cur_id = id;
                cur_sim = parsed_sim;
            }
        } else if (result.find("who?") != std::string::npos) {
            cur_known = false;
        }

        // Require same outcome for 1s before posting
        if (m_stable_first) {
            m_stable_first = false;
            m_stable_known = cur_known;
            m_stable_id = cur_id;
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
        if (now_tick - m_stable_start_tick >= one_sec) {
            bool should_send = false;
            if (!m_stable_sent) {
                should_send = true; // first send after stable
            } else if (now_tick - m_last_stable_post_tick >= pdMS_TO_TICKS(5000)) {
                should_send = true; // periodic resend every 5s while stable
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
    }

    void recognition_cleanup() override {
        who::app::WhoRecognitionAppLCD::recognition_cleanup();
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

    // LCD drawing is dynamically gated via lcd_enabled()

    // Recognition stability gating
    bool m_stable_first {true};
    bool m_stable_known {false};
    int m_stable_id {-1};
    float m_stable_sim {0.0f};
    TickType_t m_stable_start_tick {0};
    bool m_stable_sent {false};
    TickType_t m_last_stable_post_tick {0};
};