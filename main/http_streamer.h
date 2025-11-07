#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

httpd_handle_t start_webserver(void);

// Returns true while a /stream client is actively connected and being served
bool http_streaming_active(void);

// Set motion detected state (true = person detected). Provided for external tasks.
void http_motion_set_state(bool active);
// Query current motion detected state.
bool http_motion_get_state(void);

#ifdef __cplusplus
}
#endif
