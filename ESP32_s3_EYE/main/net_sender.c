#include "net_sender.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

// Existing sync senders we will call from the background task
bool post_plain_to_server(const char *ip, uint16_t port, const char *path, const char *body, size_t len);
// Combined JPEG send: converts once and sends to both Telegram and Supabase
http_success_t send_rgb565_image(const uint8_t *rgb565, uint16_t width, uint16_t height, uint8_t quality, const char *caption);

static const char *TAG = "net_sender";

typedef enum {
    NET_ITEM_HTTP_PLAIN = 1,
    NET_ITEM_TG_RGB565 = 2,
} net_item_type_t;

typedef struct {
    char *ip;
    uint16_t port;
    char *path;
    char *body;
    size_t len;
} http_plain_t;

typedef struct {
    uint8_t *rgb565;     // ownership transferred; will be free()'d after use
    size_t rgb565_len;
    uint16_t width;
    uint16_t height;
    uint8_t quality;
    char *caption;       // copied string
} tg_rgb565_t;

typedef struct {
    net_item_type_t type;
    union {
        http_plain_t http;
        tg_rgb565_t tg;
    } u;
} net_item_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

// Need to free buffers or memory will leak
static void net_item_free(net_item_t *it)
{
    if (!it) return;
    if (it->type == NET_ITEM_HTTP_PLAIN) {
        if (it->u.http.ip) free(it->u.http.ip);
        if (it->u.http.path) free(it->u.http.path);
        if (it->u.http.body) free(it->u.http.body);
    } else if (it->type == NET_ITEM_TG_RGB565) {
        if (it->u.tg.rgb565) free(it->u.tg.rgb565);
        if (it->u.tg.caption) free(it->u.tg.caption);
    }
}

static void net_sender_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "network sender task started on core %d", xPortGetCoreID());
    net_item_t item;
    while (1) {
        // Create a queue for the sending items
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Process the item based on its type
        switch (item.type) {
            case NET_ITEM_HTTP_PLAIN: {
                bool ok = post_plain_to_server(item.u.http.ip,
                                               item.u.http.port,
                                               item.u.http.path,
                                               item.u.http.body,
                                               item.u.http.len);
                ESP_LOGI(TAG, "HTTP plain sent: %s", ok ? "OK" : "FAIL");
                net_item_free(&item);
                break;
            }
            case NET_ITEM_TG_RGB565: {
                // send rgb565 image to telegram and supabase with single conversion
                http_success_t ok = send_rgb565_image(item.u.tg.rgb565,
                                                      item.u.tg.width,
                                                      item.u.tg.height,
                                                      item.u.tg.quality,
                                                      item.u.tg.caption ? item.u.tg.caption : "");
                ESP_LOGI(TAG, "Telegram photo sent: %s", ok.telegram_ok ? "OK" : "FAIL");
                ESP_LOGI(TAG, "Supabase photo sent: %s", ok.supabase_ok ? "OK" : "FAIL");
                // We must free buffers
                net_item_free(&item);
                break;
            }
            default:
                ESP_LOGW(TAG, "Unknown net item type: %d", (int)item.type);
                net_item_free(&item);
                break;
        }
        // Yield briefly to avoid monopolizing a core if queue is busy but normally its okay as only sending is on Core 1
        vTaskDelay(1);
    }
}

bool net_sender_start(int core_id)
{
    // simple validation checks 
    if (s_task) return true;
    if (core_id != 0 && core_id != 1) core_id = 1;
    if (!s_queue) {
        // Depth 6: small bounded buffer to avoid memory pressure
        s_queue = xQueueCreate(6, sizeof(net_item_t));
        if (!s_queue) {
            ESP_LOGE(TAG, "failed to create queue");
            return false;
        }
    }
    // create stack for TLS. During testing, if the stack is too small, mbedTLS will fail to allocate memory and unable to send data
    const uint32_t stack_words = 6144; // ~24 KB stack for TLS
    UBaseType_t prio = tskIDLE_PRIORITY + 2; // moderate priority
    BaseType_t rc = xTaskCreatePinnedToCore(net_sender_task, "net_sender", stack_words, NULL, prio, &s_task, core_id);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create net_sender task");
        s_task = NULL;
        return false;
    }
    return true;
}

// Duplicate a string with malloc
static char *strdup_n(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
// For all data send, we create a copy of the data to send, so that we can release the frame faster. Uses more memory but better responsiveness.
bool net_send_http_plain_async(const char *ip,
                               uint16_t port,
                               const char *path,
                               const char *body,
                               size_t len)
{
    if (!ip || !path || !body || len == 0) return false;
    if (!s_queue) {
        // Try lazy-start on core 1
        if (!net_sender_start(1)) return false;
    }
    net_item_t it = {0};
    it.type = NET_ITEM_HTTP_PLAIN;
    it.u.http.ip = strdup_n(ip);
    it.u.http.path = strdup_n(path);
    it.u.http.port = port;
    it.u.http.body = (char *)malloc(len);
    it.u.http.len = len;
    if (!it.u.http.ip || !it.u.http.path || !it.u.http.body) {
        net_item_free(&it);
        return false;
    }
    memcpy(it.u.http.body, body, len);
    if (xQueueSend(s_queue, &it, 0) != pdPASS) {
        net_item_free(&it);
        return false;
    }
    return true;
}

bool net_send_telegram_rgb565_take(uint8_t *rgb565,
                                   size_t rgb565_len,
                                   uint16_t width,
                                   uint16_t height,
                                   uint8_t quality,
                                   const char *caption)
{
    if (!rgb565 || rgb565_len == 0 || width == 0 || height == 0) return false;
    if (!s_queue) {
        if (!net_sender_start(1)) return false;
    }
    net_item_t it = {0};
    it.type = NET_ITEM_TG_RGB565;
    it.u.tg.rgb565 = rgb565; // take ownership
    it.u.tg.rgb565_len = rgb565_len;
    it.u.tg.width = width;
    it.u.tg.height = height;
    it.u.tg.quality = quality;
    it.u.tg.caption = caption ? strdup_n(caption) : NULL;
    if (caption && !it.u.tg.caption) {
        net_item_free(&it);
        return false;
    }
    if (xQueueSend(s_queue, &it, 0) != pdPASS) {
        net_item_free(&it);
        return false;
    }
    return true;
}
