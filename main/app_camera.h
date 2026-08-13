/**
 * @file    app_camera.h
 * @brief   ESP32-CAM UART 拍照驱动接口 (S7)
 */

#ifndef APP_CAMERA_H
#define APP_CAMERA_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 UART2 (GPIO16/17, 115200) 与 ESP32-CAM 通信
 * @return ESP_OK on success
 */
esp_err_t Camera_Init(void);

/**
 * @brief 触发 ESP32-CAM 拍摄一张照片
 * @param jpeg  输出: JPEG 数据指针 (malloc 分配, 调用者须 Camera_Free)
 * @param len   输出: JPEG 数据长度
 * @return ESP_OK on success
 *
 * 协议: 发送 "CAPTURE\n" → 读 "SIZE:XXXXX\n" → 读 XXXXX 字节 → 读 "DONE\n"
 */
esp_err_t Camera_Capture(uint8_t **jpeg, size_t *len);

/**
 * @brief 释放上次拍摄的照片缓冲区
 */
void Camera_Free(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAMERA_H */
