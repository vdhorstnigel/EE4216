#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG_HTTP = "http_sender";

bool post_plain_to_server(const char *ip,
                          uint16_t port,
                          const char *path,
                          const char *body,
                          size_t len)
{
    if (!ip || !path || !body || len == 0) return false;

    char url[192];
    // Ensure path starts with '/'
    const char *p = path;
    char path_buf[96];
    if (path[0] != '/') {
        snprintf(path_buf, sizeof(path_buf), "/%s", path);
        p = path_buf;
    }
    snprintf(url, sizeof(url), "http://%s:%u%s", ip, (unsigned)port, p);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_post_field(client, body, (int)len);

    // Perform with a small retry on transient EAGAIN/timeout
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        err = esp_http_client_perform(client);
        if (err == ESP_OK) break;
        if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG_HTTP, "perform attempt %d failed: %s (retrying)", attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG_HTTP, "perform failed: %s (len=%d)", esp_err_to_name(err), (int)len);
        esp_http_client_cleanup(client);
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG_HTTP, "POST %s -> %d (%d bytes)", url, status, content_len);
    esp_http_client_cleanup(client);
    return status >= 200 && status < 300;
}
