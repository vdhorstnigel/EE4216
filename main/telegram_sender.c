#include "esp_http_client.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "img_converters.h"
#include "esp_crt_bundle.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "net_sender.h"

static const char *TAG = "telegram_sender";

extern const char *Telegram_Bot_Token;
extern const char *Telegram_Chat_ID;
extern const char *telegram_cert;

extern const char *SUPABASE_URL;
extern const char *SUPABASE_SERVICE_KEY;
extern const char *BUCKET;


// Common HTTP client setup for Telegram POST
static inline void setup_http_client(esp_http_client_handle_t h, const char *ctype)
{
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    esp_http_client_set_header(h, "Content-Type", ctype);
    esp_http_client_set_header(h, "User-Agent", "esp-idf-telegram/1.0");
}

// Function to write all data to HTTP client
static bool http_write_all(esp_http_client_handle_t client, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    int total = 0;
    while (total < len) {
        int n = esp_http_client_write(client, (const char *)p + total, len - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// Send a JPEG buffer to Telegram using sendPhoto multipart/form-data
static bool send_jpeg_to_telegram(const uint8_t *jpg, size_t jpg_len, const char *caption) {
    if (!jpg || jpg_len == 0 || !Telegram_Bot_Token || !Telegram_Chat_ID) {
        ESP_LOGE(TAG, "bad args or missing credentials");
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", Telegram_Bot_Token);

    const char *boundary = "------------------------7e13971310874b5f"; // static boundary

    char part1[256];
    int part1_len = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n",
        boundary, Telegram_Chat_ID);

    char part2_hdr[256];
    int part2_hdr_len = snprintf(part2_hdr, sizeof(part2_hdr),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary);

    char part3_cap[256];
    int part3_cap_len = 0;
    if (caption && caption[0]) {
        part3_cap_len = snprintf(part3_cap, sizeof(part3_cap),
            "\r\n--%s\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
            "%s",
            boundary, caption);
    }

    char closing[64];
    int closing_len = snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", boundary);

    size_t content_length = (size_t)part1_len + (size_t)part2_hdr_len + jpg_len + (size_t)part3_cap_len + (size_t)closing_len;

    esp_http_client_handle_t client = NULL;
    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);

    esp_err_t err = ESP_FAIL;
    const int max_attempts = 3;

    // Used telegram cert for SSL verification
    esp_http_client_config_t cfg_pinned = {
        .url = url,
        .cert_pem = telegram_cert,
        .timeout_ms = 30000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .keep_alive_enable = false,
        .buffer_size_tx = 1024,
        .buffer_size = 1024,
    };
    client = esp_http_client_init(&cfg_pinned);
    if (client) {
        setup_http_client(client, ctype);
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            err = esp_http_client_open(client, content_length);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "http_open (pinned) attempt %d/%d failed: %s", attempt, max_attempts, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
        }
        if (err != ESP_OK) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }
    }

    // If no client connection available, abort sending
    if (client == NULL) {
        ESP_LOGE(TAG, "No HTTP client connection available, aborting send");
        return false;
    }

    // send data in parts to prevent large blocking write and buffer issues
    bool ok = http_write_all(client, part1, part1_len)
           && http_write_all(client, part2_hdr, part2_hdr_len)
           && http_write_all(client, jpg, (int)jpg_len);
    if (ok && part3_cap_len > 0) ok = http_write_all(client, part3_cap, part3_cap_len);
    if (ok) ok = http_write_all(client, closing, closing_len);

    if (!ok) {
        ESP_LOGE(TAG, "http_write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool success = (status == 200);
    if (!success) {
        ESP_LOGE(TAG, "Telegram sendPhoto failed, HTTP %d", status);
        if (status == 400) {
        // For 400 (Bad Request), verify your chat_id and that you started the bot.
        }
    } else {
        ESP_LOGI(TAG, "Telegram sendPhoto OK (%u bytes)", (unsigned)jpg_len);
    }
    return success;
}

// pretty much the same as send_jpeg_to_telegram but for Supabase storage upload, using different HTTP method and headers
static bool send_jpeg_to_supabase(const uint8_t *jpg, size_t jpg_len, const char *caption)
{
    if (!jpg || jpg_len == 0 || !SUPABASE_URL || !SUPABASE_SERVICE_KEY || !BUCKET) {
        ESP_LOGE(TAG, "bad args or missing Supabase credentials");
        return false;
    }
    const char* filename = "motion.jpg";

    char url[256];
    int n = snprintf(url, sizeof(url), "%s/storage/v1/object/%s/%s", SUPABASE_URL, BUCKET, filename);
    if (n <= 0 || n >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "URL too long");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach, // rely on IDF certificate bundle
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "apikey", SUPABASE_SERVICE_KEY);

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", SUPABASE_SERVICE_KEY);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, (int)jpg_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int written = esp_http_client_write(client, (const char *)jpg, (int)jpg_len);
    if (written != (int)jpg_len) {
        ESP_LOGE(TAG, "http write failed %d/%d", written, (int)jpg_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    // Read response body (often contains helpful JSON error message)
    char resp[256];
    int rlen = esp_http_client_read(client, resp, sizeof(resp) - 1);
    if (rlen > 0) {
        resp[rlen] = '\0';
        ESP_LOGI(TAG, "Supabase response: %s", resp);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Supabase upload failed, HTTP %d", status);
        return false;
    }

    char pub[256];
    int m = snprintf(pub, sizeof(pub), "%s/storage/v1/object/public/%s/%s", SUPABASE_URL, BUCKET, filename);
    if (m > 0 && m < (int)sizeof(pub)) {
        ESP_LOGI(TAG, "Supabase image uploaded: %s", pub);
    } else {
        ESP_LOGI(TAG, "Supabase image uploaded: %s/...", filename);
    }
    return true;
}

http_success_t send_rgb565_image(const uint8_t *rgb565,
                                             uint16_t width,
                                             uint16_t height,
                                             uint8_t quality,
                                             const char *caption)
{
    http_success_t result = { .telegram_ok = false, .supabase_ok = false };
    if (!rgb565 || width == 0 || height == 0) {
        return result;
    }
    // create buffer for JPEG conversion
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    size_t src_len = (size_t)width * height * 2;
    // convert to JPEG
    bool conv = fmt2jpg((uint8_t *)rgb565, src_len, width, height, PIXFORMAT_RGB565, quality, &jpg_buf, &jpg_len);
    if (!conv || !jpg_buf) {
        ESP_LOGE(TAG, "fmt2jpg failed");
        return result;
    }
    // send the JPEG buffer to Telegram
    result.telegram_ok = send_jpeg_to_telegram(jpg_buf, jpg_len, caption);
    // send the JPEG buffer to Supabase
    result.supabase_ok = send_jpeg_to_supabase(jpg_buf, jpg_len, caption);
    // Free the JPEG buffer
    free(jpg_buf);
    return result;
}
