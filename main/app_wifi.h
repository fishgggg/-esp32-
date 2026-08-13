/**
 * @file    app_wifi.h
 * @brief   WiFi SoftAP 初始化 + NVS 配置管理
 */

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS，从 NVS 读取配置或写入默认值
 * @return ESP_OK on success
 */
esp_err_t config_init(void);

/**
 * @brief 启动 WiFi SoftAP (SSID/PWD 从 NVS 读取，失败则用默认值)
 * @return ESP_OK on success
 */
esp_err_t wifi_init_ap(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WIFI_H */
