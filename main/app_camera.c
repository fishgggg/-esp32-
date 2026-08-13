/**
 * @file    app_camera.c
 * @brief   ESP32-CAM UART 拍照驱动 — UART2 通信协议 (S7)
 *
 * 协议 (UART2 115200 8N1):
 *   主控 → CAM: "CAPTURE\n"
 *   CAM  → 主控: "SIZE:12345\n"
 *   CAM  → 主控: 12345 bytes JPEG binary
 *   CAM  → 主控: "DONE\n"
 */

#include "app_camera.h"
#include "app_core.h"

#include "esp_log.h"
#include "driver/uart.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "camera";

/* UART 配置 */
#define CAM_UART_BAUD   115200
#define CAM_UART_BUF_SZ 2048
#define CAM_TIMEOUT_MS  5000
#define JPEG_MAX_SIZE   (64 * 1024)  /* 64KB max */

static uint8_t *g_jpeg_buf = NULL;

/* ================================================================
 * UART2 初始化
 * ================================================================ */
esp_err_t Camera_Init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = CAM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t ret = uart_driver_install(CAM_UART_NUM, CAM_UART_BUF_SZ, CAM_UART_BUF_SZ, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART2 driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(CAM_UART_NUM, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART2 param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(CAM_UART_NUM, CAM_UART_TX, CAM_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART2 set pins failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "UART2 ready (TX=%d RX=%d, %d baud)", CAM_UART_TX, CAM_UART_RX, CAM_UART_BAUD);
    return ESP_OK;
}

/* ================================================================
 * 读取一行 (以 \n 结尾), 超时返回 -1
 * ================================================================ */
static int uart_read_line(char *buf, int max_len, int timeout_ms)
{
    int idx = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (idx < max_len - 1) {
        if (xTaskGetTickCount() > deadline) {
            return -1;  /* 超时 */
        }
        uint8_t ch;
        int len = uart_read_bytes(CAM_UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            buf[idx++] = ch;
            if (ch == '\n') {
                buf[idx] = '\0';
                return idx;
            }
        }
    }
    return -1;
}

/* ================================================================
 * 拍照 — 触发 ESP32-CAM 拍摄并接收 JPEG
 * ================================================================ */
esp_err_t Camera_Capture(uint8_t **jpeg, size_t *len)
{
    if (jpeg) *jpeg = NULL;
    if (len)  *len  = 0;

    /* 清空 UART 缓冲 */
    uart_flush(CAM_UART_NUM);

    /* Step 1: 发送拍照命令 */
    const char *cmd = "CAPTURE\n";
    int sent = uart_write_bytes(CAM_UART_NUM, cmd, strlen(cmd));
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send CAPTURE command");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Capture command sent, waiting for response...");

    /* Step 2: 读取响应头 "SIZE:XXXXX\n" */
    char line[64] = {0};
    if (uart_read_line(line, sizeof(line), CAM_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Timeout waiting for SIZE header");
        return ESP_ERR_TIMEOUT;
    }
    int jpeg_size = 0;
    if (sscanf(line, "SIZE:%d", &jpeg_size) != 1 || jpeg_size <= 0 || jpeg_size > JPEG_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid SIZE header: '%s'", line);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Receiving JPEG: %d bytes", jpeg_size);

    /* Step 3: 读取 JPEG 数据 */
    if (g_jpeg_buf) { free(g_jpeg_buf); g_jpeg_buf = NULL; }
    g_jpeg_buf = malloc(jpeg_size);
    if (g_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "Malloc %d bytes failed", jpeg_size);
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CAM_TIMEOUT_MS);
    while (received < jpeg_size) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGE(TAG, "Timeout: received %d/%d bytes", received, jpeg_size);
            return ESP_ERR_TIMEOUT;
        }
        int n = uart_read_bytes(CAM_UART_NUM, g_jpeg_buf + received,
                                jpeg_size - received, pdMS_TO_TICKS(100));
        if (n > 0) received += n;
    }

    /* Step 4: 读取尾部 "DONE\n" */
    if (uart_read_line(line, sizeof(line), 2000) < 0) {
        ESP_LOGW(TAG, "No DONE trailer (photo may still be valid)");
    }

    ESP_LOGI(TAG, "JPEG captured: %d bytes", received);
    if (jpeg) *jpeg = g_jpeg_buf;
    if (len)  *len  = received;
    return ESP_OK;
}

/* ================================================================
 * 释放缓冲区
 * ================================================================ */
void Camera_Free(void)
{
    if (g_jpeg_buf) {
        free(g_jpeg_buf);
        g_jpeg_buf = NULL;
    }
}
