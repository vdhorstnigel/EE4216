// C-only implementation of MQTT v5 publish; C++ adapter wraps this for app use
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"

// Credentials are defined in credentials.c, declare here
extern const char *MQTT_Broker;
extern const char *MQTT_Broker_Username;
extern const char *MQTT_Broker_Password;
extern const char *MQTT_Detection_topic;
extern const char *ca_cert;

static const char *TAG = "mqtt5";
static esp_mqtt_client_handle_t client = NULL;

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

void mqtt5_app_start(void)
{
    if (client) {
        return;
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_Broker,
#if CONFIG_MQTT_PROTOCOL_5
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
#endif
        .network.disable_auto_reconnect = false,
        .credentials.username = MQTT_Broker_Username,
        .credentials.authentication.password = MQTT_Broker_Password,
        .broker.verification.certificate = ca_cert
    };
    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void mqtt5_publish_detection_c(int det_count)
{
    if (!client) {
        mqtt5_app_start();
        if (!client) {
            ESP_LOGW(TAG, "MQTT client not ready");
            return;
        }
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"detections\":%d}", det_count);
    int msg_id = esp_mqtt_client_publish(client, MQTT_Detection_topic, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Published (%d) to %s: %s", msg_id, MQTT_Detection_topic, payload);
}

void mqtt5_publish_payload_c(const char *payload)
{
    if (!client) {
        mqtt5_app_start();
        if (!client) {
            ESP_LOGW(TAG, "MQTT client not ready");
            return;
        }
    }
    int msg_id = esp_mqtt_client_publish(client, MQTT_Detection_topic, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Published (%d) to %s (len=%u)", msg_id, MQTT_Detection_topic, (unsigned)strlen(payload));
}
