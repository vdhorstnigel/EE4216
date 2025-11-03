#pragma once
#include "who_recognition_app_lcd.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mqtt5.h"

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
        //ESP_LOGI("Recognition", "%s", result.c_str());
    }

    // Detection results (bounding boxes, scores, timestamp, image ref)
    void detect_result_cb(const who::detect::WhoDetect::result_t& result) override {
        // keep LCD overlay behavior
        who::app::WhoRecognitionAppLCD::detect_result_cb(result);

        vTaskDelay(pdMS_TO_TICKS(200)); // check detection every 200ms
        if ((int)result.det_res.size() > 0) {
           mqtt5_publish_detection(result);
        }
        else {
            ESP_LOGI("Detection", "No detections.");
        }
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
