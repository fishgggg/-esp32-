# 01 · ESP32 开发笔记（S1-S8 主体）

## 引脚表（最终版）

| 功能 | GPIO | 说明 |
|---|---|---|
| DHT22 温湿度 | 4 | bit-bang，4.7kΩ 上拉，偶发超时 → 滑动滤波兜底 |
| PIR 人体 | 5 | HC-SR501，有人 = HIGH |
| 蜂鸣器 | 18 | **低电平触发**，启动时先拉高关闭 |
| I²C SDA / SCL | 21 / 22 | BH1750(0x23) + SSD1306(0x3C) 共享，100kHz |
| MQ-135 空气 | 34 | ADC1_CH6，5V → 10K+20K 分压 |
| MQ-2 烟雾 | 35 | ADC1_CH7，同上 |
| UART2 → CAM | 16(TX) / 17(RX) | 115200 8N1 |
| LD3320 语音 | 14/12/13/15 + 2/27 | VSPI（代码保留，已禁用） |
| LED 灯 / 窗帘 / 空调 | 32 / 33 / 25 | 高电平亮，串 100Ω~220Ω 限流 |

> Relay4 (GPIO26) 预留。步进电机已移除。

## S1 骨架
- SoftAP 启动 + **NVS 配置**（SSID/密码/Broker IP，首次启动写默认值，之后可覆盖）
- 任务：SensorTask(Core0, prio5, 4096B) / DisplayTask(Core0, prio2) / MQTTTask(Core1, prio4, 8192B) / HttpTask(Core1, prio3, 6144B)
- **I²C 总线在 main.c 初始化**，全局 `g_i2c_bus` 句柄，SensorTask/DisplayTask 共用

## S2 传感器 + 校准
- DHT22 bit-bang，滑动滤波
- **校准公式**（2026-07-03 修正，此前系数放大过度）：
  - `CO2  = 400 + adc × 3100 / 4095`（清洁空气 ADC≈124 → ~494ppm）
  - `PM25 = adc × 3000 / 4095`（清洁空气 → ~91）
- MQ 传感器预热 2-3 分钟，完全老化 24-48h

## S3 告警
- 阈值：CO2 > 1200 或 PM25 > 1500 → 蜂鸣器响
- S8 增加**3s 打盹**：手动关闭后 3s 内不因超标重复触发

## S4 MQTT
- Broker `192.168.4.2:1883`，ClientID `esp32_smarthome`，QoS 0
- 上报 `smarthome/sensor`，订阅 `smarthome/cmd`
- **SNTP**：SoftAP 无外网 → 3s 快速超时，避免默认 30s 卡死

## S5 OLED
- SSD1306 I²C 驱动（**移植自 STM32**），5×7 字体，页寻址
- SensorData **双队列**：SensorTask 同时发 MQTT + Display

## S6 LD3320（已废弃）
- **PLL 修正**（22.1184MHz 晶振，ASR 模式）：`PLL_11=0x0A, PLL_ASR_19=0x3F, PLL_ASR_1B=0x48, PLL_ASR_1D=0x1F`
- `wait_idle()` 需接受 DSP_STATUS = `0x21 || 0xFF`
- 识别始终返回 id=255 → **放弃硬件语音**，改用 Web 控制面板

## S7 Web 仪表盘 + ESP32-CAM

### Web（4 个接口）
| 路径 | 方法 | 功能 |
|---|---|---|
| `/` | GET | 嵌入式 HTML 仪表盘（暗色主题，无外部依赖） |
| `/events` | GET | SSE 流，每 2s 推 SensorData |
| `/api/control` | POST | 控制命令 `{"cmd":"buzzer","value":true}` |
| `/photo.jpg` | GET | 最新 ESP32-CAM 照片 |

### SSE 三 Bug（复盘价值最高）
1. **handler 阻塞单线程 HTTP 服务器**：`handler_events` 的 `while(1)` 心跳永久占住唯一的 httpd 线程
   → `httpd_req_async_handler_begin()` 非阻塞，心跳移到主循环 `sse_heartbeat()`
2. **队列饥饿**：MQTTTask(prio4) 优先于 HttpTask(prio3) 消费 `sensor_queue`，HttpTask peek 永远为空
   → 新增独立 `web_queue`，SensorTask 单独发给 HttpTask
3. **Transfer-Encoding: chunked 不一致**：`httpd_resp_send_chunk()` 自动加 chunked，但 `sse_broadcast()` 用 raw `send()` → 手机浏览器严格校验 → 断连重连反复横跳
   → 修复：全程 raw `send()` 手写响应头，统一无 chunked
   > **教训**：PC curl 不校验 chunked → "看起来正常"，手机浏览器才暴露。手机端必测。

### ESP32-CAM（独立项目）
- **协议**（115200）：主控发 `CAPTURE\n` → CAM 回 `SIZE:12345\n` + JPEG 数据 + `DONE\n`
- **烧录**：ESP32-CAM 无 USB 转串口 → 需外部模块（FT232/CP2102/CH340），**GPIO0 拉低进下载模式**
- **必须启用 `CONFIG_SPIRAM=y`**，否则 `esp_camera_init()` 挂死（无法分配 DMA 帧缓冲）
- XCLK=GPIO0 确认正确（改 GPIO21 后 Camera 无法检测）
- 状态 LED：慢闪(~2s)=就绪 / 一次快闪=拍照完成 / 五次快闪=初始化失败

## S8 语音/文字控制 + 设备状态同步

- **Web Speech API 国内被墙**（Google）→ **文字输入框兜底**，语音和文字共用 `matchCommand()`
- 设备状态：全局 `g_device_state`（buzzer / light / curtain / ac），SSE 每 2s 推送硬件真相
- 命令关键词：`灯+开/关`、`蜂鸣+开/关`、`窗帘+开/关`、`空调+开/关`
- 蜂鸣器打盹：`buzzer_snooze_until = now + 3s`
- LED 接线：GPIO32/33/25 → 100Ω~220Ω → LED → GND

## 任务通信架构

```
main.c 创建:
  g_i2c_bus (全局 I²C0 总线)
  sensor_queue  (SensorTask → MQTTTask)
  display_queue (SensorTask → DisplayTask)
  web_queue     (SensorTask → HttpTask, S7 防饥饿)
  cmd_queue     (HTTP/MQTT → 执行动作)

SensorTask 每 2s: 采集 → 滤波 → 告警判断 → 发多条队列
MQTTTask: 序列化 → PUBLISH smarthome/sensor + 消费 cmd_queue
HttpTask: SSE /events + POST /api/control + GET /photo.jpg
```

→ 遗留问题见 [02-known-issues.md](02-known-issues.md)
