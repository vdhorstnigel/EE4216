#include "lwip/sockets.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "tcp_sender";

bool send_jpeg_over_tcp(const char *ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in dest = {.sin_family=AF_INET, .sin_port=htons(port)};
    inet_pton(AF_INET, ip, &dest.sin_addr);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) { close(sock); return false; }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { close(sock); return false; }

    uint32_t len = htonl(fb->len);
    if (send(sock, &len, sizeof(len), 0) < 0) { esp_camera_fb_return(fb); close(sock); return false; }
    if (send(sock, fb->buf, fb->len, 0) < 0)  { esp_camera_fb_return(fb); close(sock); return false; }

    esp_camera_fb_return(fb);
    close(sock);
    ESP_LOGI(TAG, "JPEG sent");
    return true;
}