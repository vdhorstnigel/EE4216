#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "http_stream";
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *_STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t snapshot_get_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    esp_err_t res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

static esp_err_t stream_get_handler(httpd_req_t *req) {
    char part_buf[64];
    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        int hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
        httpd_resp_send_chunk(req, part_buf, hlen);
        httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        // optionally yield to let other tasks run
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {.uri="/stream", .method=HTTP_GET, .handler=stream_get_handler};
        httpd_uri_t snap_uri   = {.uri="/snapshot", .method=HTTP_GET, .handler=snapshot_get_handler};
        httpd_register_uri_handler(server, &stream_uri);
        httpd_register_uri_handler(server, &snap_uri);
    } else {
        ESP_LOGE(TAG, "Failed starting HTTP server");
    }
    return server;
}