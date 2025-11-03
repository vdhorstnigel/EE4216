#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

#include "mqtt5.h"
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
    free(out);
    return result;
}

void mqtt5_publish_detection(const who::detect::WhoDetect::result_t &result)
{
    using namespace dl::image;

    if (result.img.data == nullptr || result.img.width == 0 || result.img.height == 0) {
        ESP_LOGW(TAG_IMG, "No image to publish");
        return;
    }

    // Encode to JPEG (software implementation; works for RGB565/RGB888/GRAY)
    uint32_t caps = 0; // assume little-endian RGB565 or RGB888 in RGB order
    jpeg_img_t jpeg = sw_encode_jpeg(result.img, caps, 80);

    if (jpeg.data == nullptr || jpeg.data_len == 0) {
        ESP_LOGE(TAG_IMG, "JPEG encode failed");
        return;
    }

    // Base64 encode
    std::string b64 = base64_encode(reinterpret_cast<const uint8_t *>(jpeg.data), jpeg.data_len);
    if (b64.empty()) {
        ESP_LOGE(TAG_IMG, "Base64 encode failed");
        // Free JPEG buffer if allocated by encoder
        free(jpeg.data);
        return;
    }

    // Publish
    mqtt5_publish_payload_c(MQTT_Detection_topic, b64.c_str());

    // Free JPEG buffer (dl::image::sw_encode_jpeg allocates a buffer)
    free(jpeg.data);
}
