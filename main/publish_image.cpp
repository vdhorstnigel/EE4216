#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "who_detect.hpp"
#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"

static const char *TAG_IMG = "mqtt_img";

extern "C" {
// Declare symbols from credentials.c for C++
extern const char *MQTT_Detection_topic;
}

static std::string base64_encode(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return {};
    }
    // First call to get required length
    size_t out_len = 0;
    int rc = mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
        ESP_LOGE(TAG_IMG, "base64 size calc failed: %d", rc);
        return {};
    }
    // Allocate in PSRAM if large
    unsigned char *out = (unsigned char *)heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        // Fallback to normal heap
        out = (unsigned char *)malloc(out_len + 1);
    }
    if (!out) {
        ESP_LOGE(TAG_IMG, "OOM for base64 (%u bytes)", (unsigned)(out_len + 1));
        return {};
    }
    size_t olen = 0;
    rc = mbedtls_base64_encode(out, out_len + 1, &olen, data, len);
    if (rc != 0) {
        ESP_LOGE(TAG_IMG, "base64 encode failed: %d", rc);
        free(out);
        return {};
    }
    out[olen] = '\0';
    std::string result((char *)out, olen);
    return result;
}
