/**
 * @file    app_ld3320.h
 * @brief   LD3320 语音识别 — SPI 驱动接口 (S6)
 *
 * VSPI (SPI2): SCK=GPIO14, MISO=GPIO12, MOSI=GPIO13, CS=GPIO15
 * RST=GPIO2 (低有效), IRQ=GPIO27 (低有效, 识别完成)
 *
 * 流程: Init → AddKeywords → Start → (等待 IRQ) → GetResult → Start...
 */

#ifndef APP_LD3320_H
#define APP_LD3320_H

#include "app_core.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 关键词定义 ---- */
#define LD3320_MAX_KEYWORDS     10
#define LD3320_KEYWORD_MAX_LEN  32

typedef struct {
    uint8_t id;
    char    pinyin[LD3320_KEYWORD_MAX_LEN];   /* 拼音, 空格分隔 */
} LD3320Keyword;

/* ---- 驱动接口 ---- */

/**
 * @brief 初始化 LD3320 (VSPI + 寄存器序列)
 * @return ESP_OK on success
 */
esp_err_t LD3320_Init(void);

/**
 * @brief 添加一条关键词到识别列表
 * @param kw  关键词 (id + 拼音字符串)
 * @return     0=成功, -1=已满
 */
int LD3320_AddKeyword(const LD3320Keyword *kw);

/**
 * @brief 启动语音识别 (开始监听, 可反复调用)
 */
void LD3320_Start(void);

/**
 * @brief 查询是否有识别结果 (非阻塞)
 * @return 关键词 ID (0-255), 或 -1 表示无结果
 */
int LD3320_GetResult(void);

/**
 * @brief VoiceTask 入口 (Core 1, prio 3)
 *
 * 初始化 LD3320 → 添加关键词 → 循环识别 → 匹配后控制蜂鸣器
 */
void StartVoiceTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* APP_LD3320_H */
