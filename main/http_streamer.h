#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

httpd_handle_t start_webserver(void);
httpd_handle_t start_motion(void);

#ifdef __cplusplus
}
#endif
