#pragma once
#include "who_recognition_app_lcd.hpp"
#include "who_recognition.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "telemetry_flags.h"

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap),
          m_recognition_pending(false),
          m_detection_active(false),
          m_detection_start_tick(0),
          m_last_detection_tick(0),
          m_motion_task(nullptr),
          m_sending_telegram(false)
    {
        // Start periodic motion photo sender task
        // HTTPS + JPEG conversion needs a larger stack; use 9 KB
        xTaskCreatePinnedToCore(&MyRecognitionApp::motion_task_thunk, "MotionPhoto", 9216, this, 2, &m_motion_task, tskNO_AFFINITY);
    }

    // Expose recognition event group for web control
    EventGroupHandle_t get_recognition_event_group() const {
        return m_recognition->get_recognition_task()->get_event_group();
    }

protected:
    // Recognition result from core
    void recognition_result_cb(const std::string &result) {
        // Draw to LCD only if not in the middle of a Telegram send
        if (!m_sending_telegram) {
            who::app::WhoRecognitionAppLCD::recognition_result_cb(result);
        }
        // allow next trigger
        m_recognition_pending = false;
        ESP_LOGI("Recognition", "%s", result.c_str());
        if (result.find("id: ") != std::string::npos) {
            ESP_LOGI("Recognition", "Recognized face, resetting detection state.");
            m_detection_active = false;
            m_detection_start_tick = 0;
        }
    }

    // Detection results (bounding boxes, etc.)
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) {
        // Draw overlay only if not sending a Telegram photo to avoid SPI queue issues
        if (!m_sending_telegram) {
            who::app::WhoRecognitionAppLCD::detect_result_cb(result);
        }

        TickType_t now = xTaskGetTickCount();
        if (!result.det_res.empty()) {
            if (!m_detection_active) {
                m_detection_active = true;
                m_detection_start_tick = now;
            }
            m_last_detection_tick = now;
            // If detection sustained for >= 2s
            const TickType_t two_sec = pdMS_TO_TICKS(2000);
            if (!m_recognition_pending && (now - m_detection_start_tick >= two_sec)) {
                auto *rec_task = m_recognition->get_recognition_task();
                if (rec_task && rec_task->is_active()) {
                    xEventGroupSetBits(rec_task->get_event_group(), who::recognition::WhoRecognitionCore::RECOGNIZE);
                    m_recognition_pending = true;
                }
            }
        } else {
            // No detections in this frame; if it stays quiet for a short while, reset state
            const TickType_t quiet_ms = pdMS_TO_TICKS(300);
            if (m_detection_active && (now - m_last_detection_tick > quiet_ms)) {
                m_detection_active = false;
                m_detection_start_tick = 0;
            }
        }
    }

    void recognition_cleanup() {
        who::app::WhoRecognitionAppLCD::recognition_cleanup();
        m_recognition_pending = false;
    }

    void detect_cleanup() {
        who::app::WhoRecognitionAppLCD::detect_cleanup();
    }

private:
    bool m_recognition_pending;
    bool m_detection_active;
    TickType_t m_detection_start_tick;
    TickType_t m_last_detection_tick;
    TaskHandle_t m_motion_task;
    volatile bool m_sending_telegram;

    // Telegram caption (optional)
    static constexpr const char *MOTION_CAPTION = "Motion snapshot";

    static void motion_task_thunk(void *arg) {
        static_cast<MyRecognitionApp *>(arg)->motion_task();
    }
    void motion_task();
};

// Motion photo sender implementation to Telegram
extern "C" bool send_rgb565_image_to_telegram(const uint8_t *rgb565,
                                               uint16_t width,
                                               uint16_t height,
                                               uint8_t quality,
                                               const char *caption);

inline void MyRecognitionApp::motion_task()
{
    for (;;) {
        // Check every 5 seconds; if detection is active, send snapshot
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (m_detection_active) {
            // Get latest frame from the capture pipeline
            auto last_node = m_frame_cap->get_last_node();
            if (last_node) {
                UBaseType_t wm = uxTaskGetStackHighWaterMark(nullptr);
                ESP_LOGI("MotionPhoto", "Motion detected, sending photo to Telegram... (stack HWM: %u words)", (unsigned)wm);
                who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                if (fb && fb->buf && fb->width && fb->height) {
                    bool ok = false;
                    m_sending_telegram = true;
                    g_sending_telegram = true;
                    if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
                        ok = send_rgb565_image_to_telegram((const uint8_t *)fb->buf, fb->width, fb->height,
                                                            60, MOTION_CAPTION);
                    } else if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_JPEG) {
                        // Not expected in current pipeline, but handle generically by sending already-JPEG camera frame
                        // We can add a helper if needed; for now, skip as it requires length management
                        ok = false;
                    }
                    m_sending_telegram = false;
                    g_sending_telegram = false;
                    if (ok) {
                        ESP_LOGI("MotionPhoto", "Photo sent to Telegram");
                    } else {
                        ESP_LOGW("MotionPhoto", "Failed to send photo");
                    }
                }
            }
        }
    }
}
