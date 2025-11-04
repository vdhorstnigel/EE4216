#pragma once
#include "who_recognition_app_lcd.hpp"
#include "who_recognition.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "telemetry_flags.h"
#include <stdlib.h>

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap),
          m_recognition_pending(false),
          m_detection_active(false),
          m_detection_start_tick(0),
          m_last_detection_tick(0),
          m_motion_task(nullptr),
                    m_sending_telegram(false),
                    m_last_send_tick(0)
    {
        // Start periodic motion photo sender task
                // HTTPS + JPEG conversion needs a larger stack; use 9 KB
                // Pin to CPU0 with low priority to avoid starving detection (which runs on CPU1)
                xTaskCreatePinnedToCore(&MyRecognitionApp::motion_task_thunk, "MotionPhoto", 9216, this, 0, &m_motion_task, 0);
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
    // Suppress verbose recognition logs in production
        if (result.find("id: ") != std::string::npos) {
                // Reset detection state after recognition
            m_detection_active = false;
            m_detection_start_tick = 0;
        }
    }

    // Detection results (bounding boxes, etc.)
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) {
        // Draw overlay only if not sending a Telegram photo to avoid SPI queue issues
        ESP_LOGI("Detection", "Detection callback received with %d results", result.det_res.size());
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
    TickType_t m_last_send_tick;

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

// TCP sender for RGB565 as JPEG
extern "C" bool send_rgb565_image_over_tcp(const uint8_t *rgb565,
                                            uint16_t width,
                                            uint16_t height,
                                            const char *ip,
                                            uint16_t port,
                                            uint8_t quality);

// TCP destination from credentials.c
extern "C" const char* TCP_Server_IP;
extern "C" const unsigned short TCP_Server_Port;

// Downscale helper
extern "C" void rgb565_downscale_half(const uint16_t *src,
                                       uint16_t src_w,
                                       uint16_t src_h,
                                       uint16_t *dst);

inline void MyRecognitionApp::motion_task()
{
    for (;;) {
        // Check every 5 seconds; if detection is active, send snapshot
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Enforce a minimum interval between sends to reduce CPU/network load
        const TickType_t min_interval = pdMS_TO_TICKS(20000); // 20s
        TickType_t now = xTaskGetTickCount();
        if (m_detection_active && (now - m_last_send_tick >= min_interval)) {
            // Get latest frame from the capture pipeline
            auto last_node = m_frame_cap->get_last_node();
            if (last_node) {
                // Motion detected; send photo silently (no verbose logs)
                who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                if (fb && fb->buf && fb->width && fb->height) {
                    bool ok = false;
                    m_sending_telegram = true;
                    g_sending_telegram = true;
                    if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
                        // Downscale to half resolution to speed up JPEG encoding and transfer
                        const uint16_t src_w = fb->width;
                        const uint16_t src_h = fb->height;
                        const uint16_t dst_w = (src_w / 2);
                        const uint16_t dst_h = (src_h / 2);
                        const bool do_scale = (dst_w >= 80 && dst_h >= 60); // avoid too tiny frames
                        const uint16_t *src = (const uint16_t *)fb->buf;
                        uint16_t *tmp = nullptr;
                        const uint8_t quality = 30; // lower for faster encode and smaller payload
                        if (do_scale) {
                            tmp = (uint16_t *)malloc((size_t)dst_w * dst_h * 2);
                            if (tmp) {
                                rgb565_downscale_half(src, src_w, src_h, tmp);
                            }
                        }
                        const uint16_t *send_buf = (tmp ? tmp : src);
                        const uint16_t send_w = (tmp ? dst_w : src_w);
                        const uint16_t send_h = (tmp ? dst_h : src_h);
                        // Prefer TCP snapshot sender if configured, else fallback to Telegram
                        if (TCP_Server_IP && TCP_Server_IP[0] && TCP_Server_Port) {
                            ok = send_rgb565_image_over_tcp((const uint8_t *)send_buf, send_w, send_h,
                                                            TCP_Server_IP, TCP_Server_Port, quality);
                        } else {
                            ok = send_rgb565_image_to_telegram((const uint8_t *)send_buf, send_w, send_h,
                                                                quality, MOTION_CAPTION);
                        }
                        if (tmp) free(tmp);
                    } else if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_JPEG) {
                        // Not expected in current pipeline, but handle generically by sending already-JPEG camera frame
                        // We can add a helper if needed; for now, skip as it requires length management
                        ok = false;
                    }
                    m_sending_telegram = false;
                    g_sending_telegram = false;
                    if (ok) {
                        m_last_send_tick = now;
                    }
                    (void)ok; // silent on success; optionally handle failure
                }
            }
        }
    }
}
