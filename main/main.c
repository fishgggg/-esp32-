/**
 * @file    main.c
 * @brief   SmartHome ESP32 — 主入口
 *
 * 启动顺序:
 *   1. NVS 初始化 + 默认配置写入
 *   2. WiFi SoftAP 启动 (SSID "SmartHome")
 *   3. 创建 3 个 FreeRTOS 任务 (Sensor / Display / MQTT)
 *
 * 任务分配:
 *   SensorTask   — Core 0, prio 5, 2s 周期传感器采集 + 告警
 *   DisplayTask  — Core 0, prio 2, 事件驱动 OLED 刷新
 *   MQTTTask     — Core 1, prio 4, MQTT 收发循环
 */

#include "app_core.h"
#include "app_wifi.h"
#include "app_sensor.h"
#include "app_mqtt.h"
#include "app_display.h"
#include "app_ld3320.h"
#include "app_web.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "main";

/* ---- 全局 I²C0 总线句柄 (SensorTask + DisplayTask 共享) ---- */
i2c_master_bus_handle_t g_i2c_bus = NULL;

/* ---- 全局队列句柄 (HTTP + MQTT 互访) ---- */
void *g_sensor_queue = NULL;
void *g_cmd_queue    = NULL;
DeviceState g_device_state = {0};  /* S8: 全局设备状态 */

/* ---- 任务栈大小 ---- */
#define STACK_SENSOR    4096
#define STACK_DISPLAY   4096
#define STACK_MQTT      8192
#define STACK_VOICE     4096

/* ---- 任务优先级 ---- */
#define PRIO_SENSOR     5
#define PRIO_DISPLAY    2
#define PRIO_MQTT       4
#define PRIO_VOICE      3

/* ---- FreeRTOS 任务句柄 (保留供后续使用) ---- */
static TaskHandle_t hSensorTask  = NULL;
static TaskHandle_t hDisplayTask = NULL;
static TaskHandle_t hMQTTTask    = NULL;

/* ================================================================ */
void app_main(void)
{
    /* ---- 0. 提前拉高蜂鸣器 (防止烧录/复位期间乱响) ---- */
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 1);  /* HIGH = 关 */

    /* ---- 1. NVS + 默认配置 ---- */
    ESP_LOGI(TAG, "=== SmartHome ESP32 S1 Boot ===");
    ESP_ERROR_CHECK(config_init());

    /* ---- 2. WiFi SoftAP ---- */
    ESP_ERROR_CHECK(wifi_init_ap());

    /* ---- 2.5 I²C0 总线初始化 (BH1750 + SSD1306 共享) ---- */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port                 = I2C_MASTER_NUM,
        .sda_io_num               = I2C_MASTER_SDA_IO,
        .scl_io_num               = I2C_MASTER_SCL_IO,
        .clk_source               = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt        = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &g_i2c_bus));
    ESP_LOGI(TAG, "I2C0 bus initialized (SDA=%d, SCL=%d)",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);

    /* ---- 2.6 SensorData 队列 (SensorTask → MQTTTask) ---- */
    QueueHandle_t sensor_queue = xQueueCreate(5, sizeof(SensorData));
    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor_queue!");
    } else {
        ESP_LOGI(TAG, "sensor_queue created (depth=5)");
        g_sensor_queue = sensor_queue;
    }

    /* ---- 2.7 SensorData 队列 (SensorTask → DisplayTask) ---- */
    QueueHandle_t display_queue = xQueueCreate(5, sizeof(SensorData));
    if (display_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create display_queue!");
    } else {
        ESP_LOGI(TAG, "display_queue created (depth=5)");
    }

    /* ---- 2.8 ControlCmd 队列 (HTTP/MQTT → 执行动作) ---- */
    QueueHandle_t cmd_queue = xQueueCreate(5, sizeof(ControlCmd));
    if (cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create cmd_queue!");
    } else {
        ESP_LOGI(TAG, "cmd_queue created (depth=5)");
        g_cmd_queue = cmd_queue;
    }

    /* ---- 2.9 SensorData 队列 (SensorTask → HttpTask, 独立队列 S7 fix) ---- */
    QueueHandle_t web_queue = xQueueCreate(5, sizeof(SensorData));
    if (web_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create web_queue!");
    } else {
        ESP_LOGI(TAG, "web_queue created (depth=5)");
    }

    /* ---- 2.8 SensorTask 参数 (三队列: MQTT + Display + Web) ---- */
    static SensorTaskParams sensor_params = {0};
    sensor_params.mqtt_queue    = sensor_queue;
    sensor_params.display_queue = display_queue;
    sensor_params.web_queue     = web_queue;

    /* ---- 3. 创建 FreeRTOS 任务 ---- */
    ESP_LOGI(TAG, "Creating tasks...");

    BaseType_t ret;

    /* SensorTask — Core 0, 传感器采集 + 告警 */
    ret = xTaskCreatePinnedToCore(
        StartSensorTask, "sensor",
        STACK_SENSOR, (void *)&sensor_params,
        PRIO_SENSOR, &hSensorTask, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SensorTask!");
    }

    /* DisplayTask — Core 0, OLED 刷新 (低优先级) */
    ret = xTaskCreatePinnedToCore(
        StartDisplayTask, "display",
        STACK_DISPLAY, (void *)display_queue,
        PRIO_DISPLAY, &hDisplayTask, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DisplayTask!");
    }

    /* MQTTTask — Core 1, MQTT 通信 */
    ret = xTaskCreatePinnedToCore(
        StartMQTTTask, "mqtt",
        STACK_MQTT, (void *)sensor_queue,
        PRIO_MQTT, &hMQTTTask, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTTTask!");
    }

    /* HttpTask — Core 1, Web 仪表盘 + SSE + REST API (S7) */
    static HttpTaskParams http_params = {0};
    http_params.sensor_queue = web_queue;  /* 独立队列, 避免 MQTT 竞争 */
    http_params.cmd_queue    = cmd_queue;
    ret = xTaskCreatePinnedToCore(
        StartHttpTask, "http",
        HTTP_TASK_STACK, (void *)&http_params,
        HTTP_TASK_PRIO, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HttpTask!");
    }

    /* VoiceTask — Core 1, 语音识别 (已禁用, LD3320 代码保留) */
    // ret = xTaskCreatePinnedToCore(
    //     StartVoiceTask, "voice",
    //     STACK_VOICE, NULL,
    //     PRIO_VOICE, NULL, 1);
    // if (ret != pdPASS) {
    //     ESP_LOGE(TAG, "Failed to create VoiceTask!");
    // }

    /* ---- 4. 打印系统就绪信息 ---- */
    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "SoftAP: SSID=\"%s\" IP=192.168.4.1", DEFAULT_AP_SSID);
    ESP_LOGI(TAG, "Tasks: sensor(C0) display(C0) mqtt(C1) http(C1)");
    ESP_LOGI(TAG, "Web dashboard: http://192.168.4.1/");
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    /* app_main 返回后，主任务被删除，FreeRTOS 调度器继续运行 3 个任务 */
}
