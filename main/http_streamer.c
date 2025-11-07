#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <string.h>
#include "img_converters.h"
#include "recognition_control.h"

// Forward declare local handlers used in URI registration
static esp_err_t index_get_handler(httpd_req_t *req);
static esp_err_t action_get_handler(httpd_req_t *req);
static esp_err_t motion_get_handler(httpd_req_t *req);

static const char *TAG = "http_stream";
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *_STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

extern "C" bool net_send_telegram_rgb565_take(uint8_t *rgb565, size_t rgb565_len, uint16_t width, uint16_t height, uint8_t quality, const char *caption);

static const char *INDEX_HTML =
    "<!doctype html><html><head><meta name=viewport content='width=device-width, initial-scale=1'/>"
    "<title>ESP Stream</title>"
    "<style>body{font-family:sans-serif;margin:0;padding:0}header{padding:8px;background:#222;color:#fff}"
    "main{padding:8px}iframe#vid{width:100%;height:60vh;border:1px solid #ccc}button{margin:6px;padding:8px 12px}"
    "#bar{position:fixed;bottom:0;left:0;right:0;background:#f5f5f5;padding:8px;border-top:1px solid #ddd}"
    "</style></head><body>"
    "<header><h3>ESP Camera Stream</h3></header><main>"
    "<iframe id='vid' src='/stream' frameborder='0'></iframe>"
    "</main><div id=bar>"
    "<span id=msg style='margin-left:12px;color:#555'></span>"
    "</div><script>"
    "const msgEl=document.getElementById('msg');"
    "function go(cmd){"
    "  msgEl.textContent='sending...';"
    "  fetch('/action?cmd='+cmd,{cache:'no-store'})"
    "    .then(r=>r.text())"
    "    .then(t=>{msgEl.textContent=t;})"
    "    .catch(e=>{msgEl.textContent='error';});"
    "}"
    "</script>"
    "</body></html>";

static esp_err_t stream_get_handler(httpd_req_t *req) {
    char part_buf[64];
    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    ESP_LOGI(TAG, "stream: client connected");
    s_streaming_active = true;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "stream: fb_get failed");
            break;
        }

        esp_err_t res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        if (fb->format == PIXFORMAT_JPEG) {
            int hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
            if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) {
                esp_camera_fb_return(fb);
                break;
            }
        } else {
            // Convert on the fly and stream via callback to avoid big allocs
            // Simpler: encode to full buffer to know length (small quality to keep CPU/memory low)
            uint8_t *jpg_buf = NULL; size_t jpg_len = 0;
            bool ok = frame2jpg(fb, 60, &jpg_buf, &jpg_len);
            if (!ok || !jpg_buf) {
                esp_camera_fb_return(fb);
                ESP_LOGE(TAG, "stream: frame2jpg failed");
                break;
            }
            int hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, (unsigned)jpg_len);
            if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len) != ESP_OK) {
                free(jpg_buf);
                esp_camera_fb_return(fb);
                break;
            }
            free(jpg_buf);
        }

        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // Terminate the response
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "stream: client disconnected");
    s_streaming_active = false;
    return ESP_OK;
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri  = {.uri="/", .method=HTTP_GET, .handler=index_get_handler, .user_ctx=NULL};
    httpd_uri_t stream_uri = {.uri="/stream", .method=HTTP_GET, .handler=stream_get_handler, .user_ctx=NULL};
    httpd_uri_t motion_uri = {.uri="/motion", .method=HTTP_GET, .handler=motion_get_handler, .user_ctx=NULL};
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &stream_uri);
        httpd_register_uri_handler(server, &motion_uri);
    } else {
        ESP_LOGE(TAG, "Failed starting HTTP server");
    }
    return server;
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t motion_get_handler(httpd_req_t *req) {
    auto last_node = m_frame_cap->get_last_node();
                if (last_node) {
                    who::cam::cam_fb_t *fb = last_node->cam_fb_peek(-1);
                    if (fb && fb->buf && fb->width && fb->height) {
                        const size_t rgb_len = (size_t)fb->width * fb->height * 2;
                        uint8_t *copy = (uint8_t *)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!copy) copy = (uint8_t *)malloc(rgb_len);
                        if (copy) {
                            memcpy(copy, fb->buf, rgb_len);
                            char caption[64];
                            strncpy(caption, "Motion Detected", sizeof(caption) - 1);
                            caption[sizeof(caption) - 1] = '\0';
                            if (net_send_telegram_rgb565_take(copy, rgb_len, fb->width, fb->height, 40, caption)) {
                                m_last_send_tick = now;
                            } else {
                                // Fallback: free copy (since queue didn't take ownership)
                                free(copy);
                            }
                        } else {
                            ESP_LOGE("Detection", "Alloc snapshot buffer failed (%ux%u)", fb->width, fb->height);
                        }
                    }

            }
    return ESP_OK;
}
