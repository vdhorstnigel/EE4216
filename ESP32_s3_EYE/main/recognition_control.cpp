#include "recognition_control.h"
#include "who_recognition.hpp"

static EventGroupHandle_t s_recog_group = nullptr;

extern "C" void recognition_register_event_group(EventGroupHandle_t group) {
    s_recog_group = group;
}

extern "C" void recognition_request_recognize(void) {
    if (s_recog_group) {
        xEventGroupSetBits(s_recog_group, who::recognition::WhoRecognitionCore::RECOGNIZE);
    }
}

extern "C" void recognition_request_enroll(void) {
    if (s_recog_group) {
        xEventGroupSetBits(s_recog_group, who::recognition::WhoRecognitionCore::ENROLL);
    }
}

extern "C" void recognition_request_clear_all(void) {
    if (s_recog_group) {
        xEventGroupSetBits(s_recog_group, who::recognition::WhoRecognitionCore::CLEAR_ALL);
    }
}
