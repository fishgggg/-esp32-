/**
 * @file    app_ssd1306.h
 * @brief   SSD1306 128x64 OLED I²C 驱动 (S5)
 *
 * API:
 *   SSD1306_Init(bus)            — 初始化 OLED, 注册到 I²C 总线
 *   SSD1306_Clear()              — 清空帧缓冲
 *   SSD1306_WriteString(col, page, str, invert) — 在指定位置写 ASCII 字符串
 *   SSD1306_Update()             — 将帧缓冲刷新到 OLED
 *
 * 每字符 6×8 像素 (5×7 + 列间距), 128×64 = 21 字符 × 8 页
 */

#ifndef APP_SSD1306_H
#define APP_SSD1306_H

#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_ADDR  0x3C
#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES    8

/**
 * @brief 初始化 SSD1306 OLED
 * @param bus  I²C 总线句柄 (已由 main.c 创建)
 */
void SSD1306_Init(i2c_master_bus_handle_t bus);

/**
 * @brief 清空内部帧缓冲 (不刷新到屏幕)
 */
void SSD1306_Clear(void);

/**
 * @brief 在指定位置写字符串 (写入帧缓冲)
 * @param col    起始列 (0-20, 每个字符占 6px)
 * @param page   页 (0-7, 每页 8px 高)
 * @param str    ASCII 字符串
 * @param invert 0=正常, 1=反色
 */
void SSD1306_WriteString(uint8_t col, uint8_t page,
                         const char *str, uint8_t invert);

/**
 * @brief 将帧缓冲完整刷新到 OLED (I²C 写入)
 */
void SSD1306_Update(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SSD1306_H */
