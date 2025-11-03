#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void mqtt5_app_start(void);
// C API used by C++ wrapper
void mqtt5_publish_detection_c(int det_count);
// Generic C API to publish a text payload to a topic
void mqtt5_publish_payload_c(const char *topic, const char *payload);

#ifdef __cplusplus
}
#include "who_detect.hpp"

// C++ convenience wrapper for app code
void mqtt5_publish_detection(const who::detect::WhoDetect::result_t &result);
#endif