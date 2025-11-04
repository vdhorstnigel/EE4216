#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "img_converters.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "tcp_sender";

// Send an arbitrary JSON blob over TCP with a 4-byte big-endian length prefix
bool send_json_over_tcp(const char *ip, uint16_t port, const char *json, size_t len) {
    if (!ip || !json || len == 0) return false;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return false;
    }
    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, ip, &dest.sin_addr);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect(%s:%u) failed: errno=%d", ip, (unsigned)port, errno);
        close(sock);
        return false;
    }
    uint32_t net_len = htonl((uint32_t)len);
    ssize_t n1 = send(sock, &net_len, sizeof(net_len), 0);
    ssize_t n2 = -1;
    if (n1 == sizeof(net_len)) {
        n2 = send(sock, json, len, 0);
    }
    bool ok = (n1 == sizeof(net_len) && n2 == (ssize_t)len);
    if (!ok) {
        ESP_LOGE(TAG, "send JSON failed: n1=%d n2=%d errno=%d", (int)n1, (int)n2, errno);
    } else {
        ESP_LOGI(TAG, "JSON sent (%u bytes)", (unsigned)len);
    }
    close(sock);
    return ok;
}

bool send_jpeg_over_tcp(const char *ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return false;
    }
    struct sockaddr_in dest = {.sin_family=AF_INET, .sin_port=htons(port)};
    inet_pton(AF_INET, ip, &dest.sin_addr);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect(%s:%u) failed: errno=%d", ip, (unsigned)port, errno);
        close(sock);
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { ESP_LOGE(TAG, "esp_camera_fb_get failed"); close(sock); return false; }

    const uint8_t *send_buf = fb->buf;
    size_t send_len = fb->len;
    uint8_t *conv_buf = NULL;

    if (fb->format != PIXFORMAT_JPEG) {
        // Convert to JPEG
        if (!frame2jpg(fb, 70, &conv_buf, &send_len) || !conv_buf) {
            esp_camera_fb_return(fb);
            close(sock);
            ESP_LOGE(TAG, "JPEG conversion failed");
            return false;
        }
        send_buf = conv_buf;
    }

    uint32_t net_len = htonl((uint32_t)send_len);
    ssize_t n1 = send(sock, &net_len, sizeof(net_len), 0);
    ssize_t n2 = -1;
    if (n1 == sizeof(net_len)) {
        n2 = send(sock, send_buf, send_len, 0);
    }
    bool ok = (n1 == sizeof(net_len) && n2 == (ssize_t)send_len);
    if (!ok) {
        ESP_LOGE(TAG, "send failed: n1=%d n2=%d errno=%d", (int)n1, (int)n2, errno);
    }

    if (conv_buf) free(conv_buf);
    esp_camera_fb_return(fb);
    close(sock);
    if (ok) ESP_LOGI(TAG, "JPEG sent (%u bytes)", (unsigned)send_len);
    return ok;
}

// Send an RGB565 image buffer as JPEG over TCP
bool send_rgb565_image_over_tcp(const uint8_t *rgb565,
                                uint16_t width,
                                uint16_t height,
                                const char *ip,
                                uint16_t port,
                                uint8_t quality)
{
    if (!rgb565 || width == 0 || height == 0) return false;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "socket() failed: errno=%d", errno); return false; }
    struct sockaddr_in dest = {.sin_family=AF_INET, .sin_port=htons(port)};
    inet_pton(AF_INET, ip, &dest.sin_addr);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect(%s:%u) failed: errno=%d", ip, (unsigned)port, errno);
        close(sock);
        return false;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    size_t src_len = (size_t)width * height * 2;
    bool ok = fmt2jpg((uint8_t *)rgb565, src_len, width, height, PIXFORMAT_RGB565, quality, &jpg_buf, &jpg_len);
    if (!ok || !jpg_buf) {
        close(sock);
        ESP_LOGE(TAG, "fmt2jpg failed");
        return false;
    }

    uint32_t net_len = htonl((uint32_t)jpg_len);
    ssize_t m1 = send(sock, &net_len, sizeof(net_len), 0);
    ssize_t m2 = -1;
    if (m1 == sizeof(net_len)) {
        m2 = send(sock, jpg_buf, jpg_len, 0);
    }
    ok = (m1 == sizeof(net_len) && m2 == (ssize_t)jpg_len);
    if (!ok) {
        ESP_LOGE(TAG, "send failed: m1=%d m2=%d errno=%d", (int)m1, (int)m2, errno);
    }
    free(jpg_buf);
    close(sock);
    if (ok) ESP_LOGI(TAG, "RGB565->JPEG sent (%u bytes)", (unsigned)jpg_len);
    return ok;
}