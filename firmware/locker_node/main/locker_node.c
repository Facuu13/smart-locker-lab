#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "esp_timer.h"

#include <inttypes.h>


static const char *TAG_MQTT = "MQTT";
static esp_mqtt_client_handle_t s_mqtt = NULL;

// Cambiá la IP por la de tu PC:
#define MQTT_BROKER_URI "mqtt://192.168.1.11:1883"
#define TOPIC_TELEMETRY "locker/locker-01/telemetry"

static uint32_t s_seq = 0;
#define TELEMETRY_PERIOD_MS 5000



#define WIFI_SSID "quepasapatejode"
#define WIFI_PASS "losvilla08"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected. Retrying...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta done (SSID=%s)", WIFI_SSID);
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "Connected to broker");
            // publish hello
            esp_mqtt_client_publish(s_mqtt, TOPIC_TELEMETRY,
                                   "{\"msg\":\"hello from esp32\"}",
                                   0, 1, 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_MQTT, "Disconnected from broker");
            break;

        default:
            break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);

    ESP_LOGI(TAG_MQTT, "Starting MQTT: %s", MQTT_BROKER_URI);
}

static void publish_telemetry_once(void)
{
    if (!s_mqtt) return;

    char payload[128];
    int64_t ts = esp_timer_get_time() / 1000000; // segundos

    snprintf(payload, sizeof(payload),
             "{\"ts\":%" PRId64 ",\"seq\":%u,\"msg\":\"periodic\"}",
             ts, (unsigned)s_seq++);

    esp_mqtt_client_publish(s_mqtt, TOPIC_TELEMETRY, payload, 0, 1, 0);
    ESP_LOGI(TAG_MQTT, "Telemetry sent: %s", payload);
}


void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi connected! Now ticking...");
    mqtt_start();

    while (1) {
        publish_telemetry_once();
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }


}
