#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "esp_timer.h"

#include <inttypes.h>

#include "driver/gpio.h"


static const char *TAG_MQTT = "MQTT";
static esp_mqtt_client_handle_t s_mqtt = NULL;

// Cambiá la IP por la de tu PC:
#define MQTT_BROKER_URI "mqtt://192.168.1.11:1883"
#define TOPIC_TELEMETRY "locker/locker-01/telemetry"

static uint32_t s_seq = 0;
#define TELEMETRY_PERIOD_MS 5000

#define GPIO_BTN 17
#define TOPIC_EVENT "locker/locker-01/event"

#define GPIO_RELAY 2

#define TOPIC_CMD "locker/locker-01/cmd"
#define TOPIC_ACK "locker/locker-01/ack"



#define WIFI_SSID "quepasapatejode"
#define WIFI_PASS "losvilla08"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static QueueHandle_t s_btn_queue = NULL;

typedef struct {
    int64_t ts_us;
    int gpio;
} btn_event_t;

static volatile int64_t s_last_isr_us = 0;
#define ISR_DEBOUNCE_MS 150

static bool s_door_open = false;

typedef struct {
    char cmd_id[32];
    int duration_ms;
    bool unlock;   // true = unlock (pulso), false = lock (apagar)
} relay_cmd_t;

static QueueHandle_t s_relay_queue = NULL;
static bool s_relay_on = false;


static void handle_cmd(const char *data, int len);




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
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
            ESP_LOGI(TAG_MQTT, "Subscribed: %s", TOPIC_CMD);

            esp_mqtt_client_publish(s_mqtt, TOPIC_TELEMETRY,
                                   "{\"msg\":\"hello from esp32\"}",
                                   0, 1, 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_MQTT, "Disconnected from broker");
            break;
        
        case MQTT_EVENT_DATA:
            if (event->topic_len == strlen(TOPIC_CMD) &&
                strncmp(event->topic, TOPIC_CMD, event->topic_len) == 0) {
                ESP_LOGI(TAG_MQTT, "CMD recv: %.*s", event->data_len, event->data);
                handle_cmd(event->data, event->data_len);
            }
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


static void IRAM_ATTR btn_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time(); // microsegundos

    // debounce simple en ISR
    if ((now - s_last_isr_us) < (ISR_DEBOUNCE_MS * 1000)) return;
    s_last_isr_us = now;

    btn_event_t ev = {
        .ts_us = now,
        .gpio = (int)(intptr_t)arg
    };

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_btn_queue, &ev, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

static void button_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE, 
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BTN, btn_isr_handler, (void*)(intptr_t)GPIO_BTN));

    ESP_LOGI("BTN", "Button configured on GPIO %d (pull-up, negedge IRQ)", GPIO_BTN);
}


static void publish_door_event(bool open, int gpio, int64_t ts_us)
{
    if (!s_mqtt) return;

    const char *etype = open ? "door_open" : "door_closed";

    char payload[200];
    int64_t ts_s = ts_us / 1000000;

    snprintf(payload, sizeof(payload),
             "{\"type\":\"%s\",\"ts\":%" PRId64 ",\"source_gpio\":%d}",
             etype, ts_s, gpio);

    esp_mqtt_client_publish(s_mqtt, TOPIC_EVENT, payload, 0, 1, 0);
    ESP_LOGI(TAG_MQTT, "Door event sent: %s", payload);
}


static void button_task(void *arg)
{
    btn_event_t ev;

    while (1) {
        if (xQueueReceive(s_btn_queue, &ev, portMAX_DELAY)) {

            // Confirmación anti-rebote (fuera de ISR)
            vTaskDelay(pdMS_TO_TICKS(30));
            int level = gpio_get_level(ev.gpio);

            // pull-up + botón a GND:
            // level == 0 => puerta abierta
            // level == 1 => puerta cerrada
            bool new_door_open = (level == 0);

            if (!s_mqtt) {
                ESP_LOGW(TAG_MQTT, "MQTT not ready, drop door event");
                continue;
            }

            // Publicar SOLO si cambió el estado
            if (new_door_open != s_door_open) {
                s_door_open = new_door_open;
                publish_door_event(s_door_open, ev.gpio, ev.ts_us);
            }
        }
    }
}


static void relay_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level(GPIO_RELAY, 0);
    s_relay_on = false;

    ESP_LOGI("RELAY", "Relay configured on GPIO %d", GPIO_RELAY);
}

static void relay_set(bool on)
{
    s_relay_on = on;
    gpio_set_level(GPIO_RELAY, on ? 1 : 0);
}


static void publish_ack(const char *cmd_id, bool ok, const char *msg)
{
    if (!s_mqtt) return;

    char payload[220];
    int64_t ts_s = esp_timer_get_time() / 1000000;

    snprintf(payload, sizeof(payload),
             "{\"cmd_id\":\"%s\",\"ok\":%s,\"ts\":%" PRId64 ",\"msg\":\"%s\"}",
             cmd_id, ok ? "true" : "false", ts_s, msg);

    esp_mqtt_client_publish(s_mqtt, TOPIC_ACK, payload, 0, 1, 0);
    ESP_LOGI(TAG_MQTT, "ACK sent: %s", payload);
}

static void relay_task(void *arg)
{
    relay_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_relay_queue, &cmd, portMAX_DELAY)) {

            if (!s_mqtt) {
                ESP_LOGW("RELAY", "MQTT not ready, skip relay cmd");
                continue;
            }

            if (cmd.unlock) {
                ESP_LOGI("RELAY", "UNLOCK cmd_id=%s duration_ms=%d", cmd.cmd_id, cmd.duration_ms);
                relay_set(true);
                vTaskDelay(pdMS_TO_TICKS(cmd.duration_ms));
                relay_set(false);
                publish_ack(cmd.cmd_id, true, "unlock_done");
            } else {
                ESP_LOGI("RELAY", "LOCK cmd_id=%s", cmd.cmd_id);
                relay_set(false);
                publish_ack(cmd.cmd_id, true, "locked");
            }
        }
    }
}


static bool extract_str(const char *json, int len, const char *key, char *out, int out_sz)
{
    // busca: "key":"value"
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *e = memchr(p, '"', (json + len) - p);
    if (!e) return false;

    int n = (int)(e - p);
    if (n <= 0 || n >= out_sz) return false;

    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool extract_int(const char *json, int len, const char *key, int *out)
{
    // busca: "key":123
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);

    // parsea int
    int v = 0;
    if (sscanf(p, "%d", &v) == 1) {
        *out = v;
        return true;
    }
    return false;
}

static void handle_cmd(const char *data, int len)
{
    // copiar a buffer 0-terminated
    char buf[256];
    int n = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = 0;

    relay_cmd_t cmd = {0};
    cmd.duration_ms = 1500; // default

    if (!extract_str(buf, n, "cmd_id", cmd.cmd_id, sizeof(cmd.cmd_id))) {
        strncpy(cmd.cmd_id, "na", sizeof(cmd.cmd_id));
    }

    char action[32] = {0};
    if (!extract_str(buf, n, "action", action, sizeof(action))) {
        publish_ack(cmd.cmd_id, false, "missing_action");
        return;
    }

    extract_int(buf, n, "duration_ms", &cmd.duration_ms);
    if (cmd.duration_ms < 50) cmd.duration_ms = 50;
    if (cmd.duration_ms > 10000) cmd.duration_ms = 10000;

    if (strcmp(action, "unlock") == 0) {
        cmd.unlock = true;
    } else if (strcmp(action, "lock") == 0) {
        cmd.unlock = false;
    } else {
        publish_ack(cmd.cmd_id, false, "unknown_action");
        return;
    }

    if (!s_relay_queue) {
        publish_ack(cmd.cmd_id, false, "relay_queue_not_ready");
        return;
    }

    xQueueSend(s_relay_queue, &cmd, 0);
}


void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // 1) WiFi
    wifi_init_sta();
    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi connected! Now ticking...");

    // 2) MQTT
    mqtt_start();

    relay_gpio_init();

    s_relay_queue = xQueueCreate(8, sizeof(relay_cmd_t));
    xTaskCreate(relay_task, "relay_task", 4096, NULL, 5, NULL);


    // 3) Queue + GPIO + Task
    s_btn_queue = xQueueCreate(8, sizeof(btn_event_t));
    if (!s_btn_queue) {
        ESP_LOGE("BTN", "Failed to create queue");
        return;
    }

    button_gpio_init();
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    // 4) Loop vacío (o logs)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }



}
