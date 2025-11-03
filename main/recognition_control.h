#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#ifdef __cplusplus
extern "C" {
#endif

void recognition_register_event_group(EventGroupHandle_t group);
void recognition_request_recognize(void);
void recognition_request_enroll(void);
void recognition_request_clear_all(void);

#ifdef __cplusplus
}
#endif
