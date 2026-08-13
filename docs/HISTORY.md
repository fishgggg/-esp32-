# SmartHome 项目历程

> 一个"物联网远程控制居家智能控制系统"从 **STM32** 起步、因资源瓶颈迁移到 **ESP32** 并逐步完善的完整开发记录。
> 本文档用于复习项目演进。**主视角是 ESP32 版本**（当前成果），STM32 阶段作为背景与缘起介绍。

## 一页总览

| 阶段 | 平台 | 关键内容 | 状态 |
|---|---|---|---|
| ① 起点 | STM32F103C8T6 + ESP32(桥接) | 初版架构、AT 指令联网 | 已放弃（资源不足，2 个无法根治的 bug） |
| ② 迁移 | ESP32-DevKit-32E | 双板架构定型（2026-07-02） | ✅ |
| ③ 主体开发 | ESP32 + ESP32-CAM | S1-S8 里程碑 | S1-S7 ✅ / S8 🟡 |
| ④ 当前 | ESP32 | Web 仪表盘 + 语音文字控制 | CAM 硬件故障待更换 |

## 时间线

### 阶段 ① STM32 起步（背景）
- STM32F103C8T6 做主控，外接 ESP32 做 WiFi（AT 指令桥接），FreeRTOS
- 遇两个无法根治的硬伤 → 详见 [stm32-era.md](stm32-era.md)
  - `osDelay(N≥214)` 必 HardFault（内存损坏，FreeRTOS 内核审查无果）
  - USART 无硬件 FIFO，接收溢出丢数据

### 阶段 ② 迁移决策（2026-07-02）
- 64K Flash / 20K RAM 资源不足 → 换 **ESP32-DevKit-32E**（双核 240MHz / 520KB SRAM / 4MB Flash，自带 WiFi）
- ESP32-CAM 降级为纯摄像头外设，UART2 通信
- 详见 [esp32-era/00-migration.md](esp32-era/00-migration.md)

### 阶段 ③ ESP32 主体开发 S1-S8

| 阶段 | 内容 | 状态 |
|---|---|---|
| S1 | 骨架 + SoftAP + NVS + 任务调度 | ✅ |
| S2 | 传感器驱动（DHT22 / BH1750 / MQ-135 / MQ-2 / PIR） | ✅ |
| S3 | 蜂鸣器告警（CO2>1200 / PM25>1500） | ✅ |
| S4 | MQTT（esp_mqtt + cJSON + SNTP） | ✅ |
| S5 | OLED SSD1306 I²C 显示 | ✅ |
| S6 | LD3320 语音识别（后废弃，改用 Web） | ✅（已弃用） |
| S7 | Web 仪表盘（SSE + REST）+ ESP32-CAM | ✅ 代码通过；CAM 硬件故障 |
| S8 | 语音/文字控制 + 设备状态同步 | 🟡 LED 待焊接 |

- 实现细节 → [esp32-era/01-dev-notes.md](esp32-era/01-dev-notes.md)
- 遗留问题 → [esp32-era/02-known-issues.md](esp32-era/02-known-issues.md)

## 复习指引

| 想了解什么 | 看哪篇 |
|---|---|
| 项目现在做到哪 | 仓库根 [README.md](../README.md) |
| 为什么从 STM32 搬走 | [stm32-era.md](stm32-era.md) |
| 迁移架构怎么定 | [esp32-era/00-migration.md](esp32-era/00-migration.md) |
| ESP32 各阶段实现细节 | [esp32-era/01-dev-notes.md](esp32-era/01-dev-notes.md) |
| 还有哪些坑没填 | [esp32-era/02-known-issues.md](esp32-era/02-known-issues.md) |

---

*本目录内容由开发过程记忆整理而来，供复习参考。*
