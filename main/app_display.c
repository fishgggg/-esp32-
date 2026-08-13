/**
 * @file    app_display.c
 * @brief   DisplayTask — OLED 显示任务 (S5: SSD1306 I²C 驱动)
 *
 * 从 display_queue 接收 SensorData, 渲染到 SSD1306 OLED:
 *   页面 0: SmartHome
 *   页面 1: ──────────
 *   页面 2: T:25.7C H:67.0%
 *   页面 3: L:305lx CO2:404
 *   页面 4: PM:7 PIR:Y
 *   页面 5: MQTT Active
 *   页面 6-7: (空白)
 */

#include "app_display.h"
#include "app_core.h"
#include "app_ssd1306.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "display";

/* ================================================================
 * DisplayTask 主入口 (Core 0, prio 2, 事件驱动)
 * ================================================================ */
void StartDisplayTask(void *pvParameters)
{
    QueueHandle_t display_queue = (QueueHandle_t)pvParameters;

    ESP_LOGI(TAG, "DisplayTask starting on Core %d", xPortGetCoreID());

    if (display_queue == NULL) {
        ESP_LOGE(TAG, "display_queue is NULL — aborting");
        vTaskDelete(NULL);
        return;
    }

    /* ---- 初始化 SSD1306 (使用 main.c 创建的 I²C 总线) ---- */
    SSD1306_Init(g_i2c_bus);

    char line[32];   /* 足够容纳温湿度格式化字符串 */

    for (;;) {
        SensorData data;
        if (xQueueReceive(display_queue, &data,
                          pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* 超时 — 显示保持, 继续等待 */
            continue;
        }

        /* ---- 渲染帧 ---- */
        SSD1306_Clear();

        /* 第 0 行: 标题 */
        SSD1306_WriteString(0, 0, "SmartHome", 0);

        /* 第 1 行: 分隔线 */
        SSD1306_WriteString(0, 1, "--------------------", 0);

        /* 第 2 行: 温湿度 (手拆整数/小数, 避免 printf %f 拉入浮点库) */
        {
            int ti = (int)data.temperature;
            int td = (int)((data.temperature - (float)ti) * 10.0f + 0.5f);
            if (td < 0) td = -td;
            int hi = (int)data.humidity;
            int hd = (int)((data.humidity - (float)hi) * 10.0f + 0.5f);
            if (hd < 0) hd = -hd;
            snprintf(line, sizeof(line), "T:%d.%dC H:%d.%d%%",
                     ti, td, hi, hd);
        }
        SSD1306_WriteString(0, 2, line, 0);

        /* 第 3 行: 光照 + CO2 */
        snprintf(line, sizeof(line), "L:%d CO2:%d",
                 (int)data.light, (int)data.co2);
        SSD1306_WriteString(0, 3, line, 0);

        /* 第 4 行: PM2.5 + PIR */
        snprintf(line, sizeof(line), "PM:%d PIR:%c",
                 (int)data.pm25, data.motion ? 'Y' : 'N');
        SSD1306_WriteString(0, 4, line, 0);

        /* 第 5 行: 状态 */
        SSD1306_WriteString(0, 5, "MQTT Active", 0);

        /* 第 7 行: 版本 */
        SSD1306_WriteString(0, 7, "ESP32 SmartHome S5", 0);

        /* ---- 刷新到 OLED ---- */
        SSD1306_Update();

        ESP_LOGD(TAG, "OLED refreshed");
    }
}
