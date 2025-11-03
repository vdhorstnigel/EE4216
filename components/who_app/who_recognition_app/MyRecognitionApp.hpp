#pragma once
#include "who_recognition_app_lcd.hpp"
#include "who_recognition.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap),
          m_recognition_pending(false),
          m_detection_active(false),
          m_detection_start_tick(0),
          m_last_detection_tick(0),
          m_motion_task(nullptr)
    {
        // Start periodic motion photo sender task
        xTaskCreatePinnedToCore(&MyRecognitionApp::motion_task_thunk, "MotionPhoto", 3072, this, 2, &m_motion_task, tskNO_AFFINITY);
    }

    // Expose recognition event group for web control
    EventGroupHandle_t get_recognition_event_group() const {
        return m_recognition->get_recognition_task()->get_event_group();
    }

protected:
    // Recognition result from core
    void recognition_result_cb(const std::string &result) override {
        // keep LCD behavior
        who::app::WhoRecognitionAppLCD::recognition_result_cb(result);
        // allow next trigger
        m_recognition_pending = false;
        ESP_LOGI("Recognition", "%s", result.c_str());
    }

    // Detection results (bounding boxes, etc.)
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override {
        // keep LCD overlay behavior
        who::app::WhoRecognitionAppLCD::detect_result_cb(result);

        TickType_t now = xTaskGetTickCount();
        if (!result.det_res.empty()) {
            if (!m_detection_active) {
                m_detection_active = true;
                m_detection_start_tick = now;
            }
            m_last_detection_tick = now;
            // If detection sustained for >= 5s and recognition not pending, trigger recognition once
            const TickType_t five_sec = pdMS_TO_TICKS(5000);
            if (!m_recognition_pending && (now - m_detection_start_tick >= five_sec)) {
                auto *rec_task = m_recognition->get_recognition_task();
                if (rec_task && rec_task->is_active()) {
                    xEventGroupSetBits(rec_task->get_event_group(), who::recognition::WhoRecognitionCore::RECOGNIZE);
                    m_recognition_pending = true;
                }
            }
        } else {
            // No detections in this frame; if it stays quiet for a short while, reset state
            const TickType_t quiet_ms = pdMS_TO_TICKS(500);
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
    bool m_recognition_pending;
    bool m_detection_active;
    TickType_t m_detection_start_tick;
    TickType_t m_last_detection_tick;
    TaskHandle_t m_motion_task;

    // Change to your TCP receiver
    static constexpr const char *MOTION_TCP_IP = "192.168.0.10";
    static constexpr uint16_t MOTION_TCP_PORT = 5050;

    static void motion_task_thunk(void *arg) {
        static_cast<MyRecognitionApp *>(arg)->motion_task();
    }
    void motion_task();
};

// Motion photo sender implementation
extern "C" bool send_rgb565_image_over_tcp(const uint8_t *rgb565,
                                            uint16_t width,
                                            uint16_t height,
                                            const char *ip,
                                            uint16_t port,
                                            uint8_t quality);

inline void MyRecognitionApp::motion_task()
{
    const TickType_t period = pdMS_TO_TICKS(10000); // every 10 seconds
    const TickType_t active_window = pdMS_TO_TICKS(3000); // consider detection active if within last 3s
    for (;;) {
        vTaskDelay(period);
        TickType_t now = xTaskGetTickCount();
        if (m_detection_active && (now - m_last_detection_tick <= active_window)) {
            // Get latest frame from the capture pipeline
            auto last_node = m_frame_cap->get_last_node();
            if (last_node) {
                who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                if (fb && fb->buf && fb->width && fb->height) {
                    bool ok = false;
                    if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
                        ok = send_rgb565_image_over_tcp((const uint8_t *)fb->buf, fb->width, fb->height,
                                                        MOTION_TCP_IP, MOTION_TCP_PORT, 60);
                    } else if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_JPEG) {
                        // Not expected in current pipeline, but handle generically by sending already-JPEG camera frame
                        // We can add a helper if needed; for now, skip as it requires length management
                        ok = false;
                    }
                    if (ok) {
                        ESP_LOGI("MotionPhoto", "Photo sent to %s:%u", MOTION_TCP_IP, MOTION_TCP_PORT);
                    } else {
                        ESP_LOGW("MotionPhoto", "Failed to send photo");
                    }
                }
            }
        }
    }
}
