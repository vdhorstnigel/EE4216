#pragma once
#include "who_recognition_app_lcd.hpp"
#include "esp_log.h"

class MyRecognitionApp : public who::app::WhoRecognitionAppLCD {
public:
    explicit MyRecognitionApp(who::frame_cap::WhoFrameCap* frame_cap)
        : who::app::WhoRecognitionAppLCD(frame_cap) {}

protected:
    // Recognition status/messages (e.g., "id: 3 enrolled.", "who?")
    void recognition_result_cb(const std::string& result) override {
        // keep LCD behavior
        who::app::WhoRecognitionAppLCD::recognition_result_cb(result);
        // add console log
        ESP_LOGI("MyRecognition", "%s", result.c_str());
    }

    // Detection results (bounding boxes, scores, timestamp, image ref)
    void detect_result_cb(const who::detect::WhoDetect::result_t& result) override {
        // keep LCD overlay behavior
        who::app::WhoRecognitionAppLCD::detect_result_cb(result);
        // add console log
        ESP_LOGI("MyDetection", "ts=%ld.%06ld, detections=%d",
                 (long)result.timestamp.tv_sec, (long)result.timestamp.tv_usec,
                 (int)result.det_res.size());
        // Optional: iterate detections if you want more detail.
        // Note: structure of dl::detect::result_t depends on model; log fields you need here.
        // for (const auto& d : result.det_res) { ... }
    }

    void recognition_cleanup() override {
        who::app::WhoRecognitionAppLCD::recognition_cleanup();
        ESP_LOGI("MyRecognition", "Recognition cleanup");
    }

    void detect_cleanup() override {
        who::app::WhoRecognitionAppLCD::detect_cleanup();
        ESP_LOGI("MyDetection", "Detect cleanup");
    }
};