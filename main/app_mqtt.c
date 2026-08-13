/**
 * @file    app_mqtt.c
 * @brief   MQTTTask — MQTT 通信任务 (S4: esp_mqtt + cJSON)
 *
 * 启动流程:
 *   1. 从 pvParameters 获取 sensor_queue 句柄
 *   2. SNTP 时间同步 (pool.ntp.org, CST-8)
 *   3. esp_mqtt_client 初始化 → Broker 连接
 *   4. 事件回调: CONNECTED → subscribe smarthome/cmd
 *               DATA → 解析 JSON 下行指令 → 蜂鸣器控制
 *   5. 主循环: sensor_queue 接收 → cJSON 序列化 → PUBLISH
 */

#include "app_mqtt.h"
#include "app_core.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

static const char *TAG = "mqtt";

/* ================================================================
 * SNTP 时间同步 (CST-8)
 * ================================================================ */

static void sntp_sync_wait(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.smooth_sync = false;
    config.start = true;

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s — using local time",
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Waiting for SNTP time sync...");
    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(3000));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SNTP synced");
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout — using local time");
    }
}

/* ================================================================
 * 蜂鸣器定时控制 (下行指令 duration 参数)
 *
 * 注: buzzer_off_tick 被 MQTT 事件回调 (MQTT 内部任务) 写入,
 *     被本任务主循环读取, 无锁。32 位对齐读写, 最坏情况延迟一个周期。
 * ================================================================ */

static TickType_t buzzer_off_tick = 0;

static void buzzer_on(uint16_t duration_ms)
{
    gpio_set_level(BUZZER_GPIO, 0);                 /* 低电平 = 蜂鸣器响 */
    g_device_state.buzzer = true;                   /* S8: 同步状态 */
    if (duration_ms > 0) {
        buzzer_off_tick = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
        ESP_LOGI(TAG, "Buzzer ON (duration=%ums)", duration_ms);
    } else {
        buzzer_off_tick = 0;                        /* 0 = 持续响, 不自动关 */
        ESP_LOGI(TAG, "Buzzer ON (continuous)");
    }
}

static void buzzer_check_timeout(void)
{
    if (buzzer_off_tick > 0 && xTaskGetTickCount() >= buzzer_off_tick) {
        gpio_set_level(BUZZER_GPIO, 1);             /* 高电平 = 关 */
        buzzer_off_tick = 0;
        g_device_state.buzzer = false;              /* S8: 同步状态 */
        ESP_LOGI(TAG, "Buzzer OFF (timeout)");
    }
}

/* ================================================================
 * MQTT 事件处理回调 (运行在 MQTT 客户端内部任务上下文)
 * ================================================================ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        esp_mqtt_client_subscribe(event->client, MQTT_TOPIC_CMD, 0);
        ESP_LOGI(TAG, "Subscribed to %s", MQTT_TOPIC_CMD);
        break;

    case MQTT_EVENT_DATA: {
        /* 复制 payload 到 null-terminated 字符串 */
        char *payload = malloc(event->data_len + 1);
        if (payload == NULL) break;
        memcpy(payload, event->data, event->data_len);
        payload[event->data_len] = '\0';

        ESP_LOGI(TAG, "CMD ↓ %s", payload);

        cJSON *root = cJSON_Parse(payload);
        if (root == NULL) {
            ESP_LOGW(TAG, "Invalid JSON command: %s", payload);
            free(payload);
            break;
        }

        /* --- 蜂鸣器指令: {"buzzer":true,"duration":300} --- */
        cJSON *buzzer = cJSON_GetObjectItem(root, "buzzer");
        if (buzzer && cJSON_IsBool(buzzer)) {
            cJSON *dur = cJSON_GetObjectItem(root, "duration");
            uint16_t duration = (dur && cJSON_IsNumber(dur))
                                  ? (uint16_t)dur->valueint : 0;
            if (cJSON_IsTrue(buzzer)) {
                buzzer_on(duration);
            } else {
                gpio_set_level(BUZZER_GPIO, 1);
                buzzer_off_tick = 0;
                ESP_LOGI(TAG, "Buzzer OFF (cmd)");
            }
        }

        /* --- 继电器指令 (预留): {"relay":1,"state":1} --- */
        cJSON *relay = cJSON_GetObjectItem(root, "relay");
        cJSON *state = cJSON_GetObjectItem(root, "state");
        if (relay && cJSON_IsNumber(relay) && state && cJSON_IsBool(state)) {
            ESP_LOGI(TAG, "Relay #%d → %s (S7 TODO, hardware not connected)",
                     relay->valueint, cJSON_IsTrue(state) ? "ON" : "OFF");
        }

        cJSON_Delete(root);
        free(payload);
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error event");
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected — auto-reconnect pending");
        break;

    default:
        break;
    }
}

/* ================================================================
 * MQTTTask 主入口 (Core 1, prio 4)
 * ================================================================ */

void StartMQTTTask(void *pvParameters)
{
    QueueHandle_t sensor_queue = (QueueHandle_t)pvParameters;

    ESP_LOGI(TAG, "MQTTTask starting on Core %d", xPortGetCoreID());

    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "sensor_queue is NULL — aborting");
        vTaskDelete(NULL);
        return;
    }

    /* ---- 1. SNTP 时间同步 ---- */
    sntp_sync_wait();

    /* ---- 2. MQTT 客户端初始化 ---- */
    char broker_uri[64];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d",
             DEFAULT_MQTT_BROKER_IP, DEFAULT_MQTT_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = DEFAULT_MQTT_CLIENT_ID,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT client init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    ESP_LOGI(TAG, "MQTT client started — %s", broker_uri);

    /* ---- 3. 主循环: sensor_queue → JSON → PUBLISH ---- */
    for (;;) {
        SensorData data;
        if (xQueueReceive(sensor_queue, &data,
                          pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* 5s 超时无数据 — 检查蜂鸣器定时 */
            buzzer_check_timeout();
            continue;
        }

        /* 检查蜂鸣器 auto-off */
        buzzer_check_timeout();

        /* ---- 构建 JSON ---- */
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &timeinfo);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "ts", ts);
        cJSON_AddNumberToObject(root, "temp", roundf(data.temperature * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(root, "humi", roundf(data.humidity * 10.0f) / 10.0f);
        cJSON_AddNumberToObject(root, "lux",  data.light);
        cJSON_AddNumberToObject(root, "co2",  data.co2);
        cJSON_AddNumberToObject(root, "pm25", data.pm25);
        cJSON_AddBoolToObject(root, "pir", data.motion);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            int msg_id = esp_mqtt_client_publish(
                client, MQTT_TOPIC_SENSOR, json_str, 0, 0, 0);
            if (msg_id < 0) {
                ESP_LOGW(TAG, "Publish failed (msg_id=%d)", msg_id);
            } else {
                ESP_LOGI(TAG, "PUBLISH ↑ #%d %s", msg_id, json_str);
            }
            cJSON_free(json_str);
        }
        cJSON_Delete(root);
    }
}
