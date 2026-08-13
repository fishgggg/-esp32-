/**
 * @file    app_display.h
 * @brief   DisplayTask — OLED 显示任务 (S1 空壳)
 */

#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DisplayTask 入口 (Core 0, 低优先级, 事件驱动)
 *
 * S5 将实现: SSD1306 I²C 驱动 → 帧缓冲 → 刷新温湿度/光照/CO2
 */
void StartDisplayTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* APP_DISPLAY_H */
