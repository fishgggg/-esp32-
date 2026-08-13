# 00 · STM32 → ESP32 迁移决策（2026-07-02）

## 决策要点

| 项 | 结论 |
|---|---|
| 主控 | **ESP32-DevKit-32E**（双核 240MHz / 520KB SRAM / 4MB Flash） |
| 平台 | ESP-IDF v5.5.4 + FreeRTOS（SMP 双核） |
| 摄像头 | ESP32-CAM 降级为纯外设，UART2 通信 |
| SoftAP | SSID `SmartHome`，IP `192.168.4.1`，SSID/密码/Broker IP 存 **NVS**（默认值已脱敏） |
| 传感器周期 | 2s（DHT22 是瓶颈） |
| 日志 | `ESP_LOGx`，每模块独立 TAG |
| 时间 | SNTP 同步 `pool.ntp.org`（CST-8） |
| JSON / MQTT | cJSON + esp_mqtt 客户端库 |

## 硬件变更

- 继电器 + 步进电机：硬件不接，MQTT 软件接口预留
- LD3320：只做语音识别，不做播报
- 全部传感器/执行器直连 ESP32（不再经过 STM32）

## 架构定型

```
ESP32-DevKit-32E SoftAP "SmartHome" (192.168.4.1) <--WiFi--> 手机/PC 浏览器
  ├─ 传感器 + 执行器直连
  ├─ HTTP :80 → Web 仪表盘 (SSE 实时 + REST 控制)
  └─ UART2 ── ESP32-CAM (CAPTURE/SIZE/DONE 协议)
```

## 关键技术选型（迁移时敲定）

- **DHT22**：bit-bang `esp_rom_delay_us`，偶发 WiFi 干扰超时 → 滑动滤波兜底
- **ADC**：`ADC_ATTEN_DB_0`（0-1.1V，最灵敏档），MQ 传感器输出 ≤1V，5V 分压 10K+20K
- **I²C**：BH1750 + OLED 共享 GPIO21/22，同 Core 0 无需互斥锁
- **SPI**：LD3320 独占 VSPI（SPI2_HOST），CPOL=1 / CPHA=0，1MHz

## 为什么

STM32F103C8T6 资源不足 + osDelay 阈值 bug 无解；ESP32 自带 WiFi、大内存，从根上消除这两类问题。

→ 下一步：S1-S8 开发细节见 [01-dev-notes.md](01-dev-notes.md)
