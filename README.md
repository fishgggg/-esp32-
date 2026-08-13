# SmartHome ESP32

> 基于esp32的物联网远程控制居家智能控制系统，自己写着玩的，主要是为了熟悉FreeRTOS和ESP32。这个项目和stm32以及MSPM0不同，对于我来说是新的体验。
> — 原仓库 README 自述

基于 **ESP-IDF 5.5.4 + ESP32** 的智能家居控制系统：多传感器采集、本地 Web 控制面板、语音识别、摄像头接入、MQTT 云端上报，支持阈值告警。

> ⚠️ **安全声明**：Web 面板（HTTP 80）与 MQTT（1883 明文）为局域网内无认证设计，**仅限可信局域网使用**，请勿直接暴露到公网。

## 功能特性

- **多传感器采集**：DHT22（温湿度）、BH1750（光照）、MQ-135（CO₂ 估算）、MQ-2（烟雾）、PIR（人体检测）
- **智能告警**：CO₂ / 烟雾超阈值蜂鸣报警，带 3s 打盹防误报
- **Web 控制面板**：HTTP Server + SSE 每 2s 推送设备状态，可控制蜂鸣器/灯/窗帘/空调
- **语音识别**：LD3320 离线语音命令（VSPI 接口）
- **摄像头**：UART 串口接入 ESP32-CAM，Web 页面查看
- **MQTT 上报/下行**：传感器 JSON 上报 `smarthome/sensor`，订阅 `smarthome/cmd` 接收控制指令（esp_mqtt + cJSON）
- **SoftAP 热点**：设备自建热点，SSID/密码/MQTT Broker 均存于 NVS，可运行时覆盖
- **SNTP 校时**：CST-8 时区，上报数据带时间戳
- **OLED 显示**：SSD1306 实时显示传感器数据（I²C 与 BH1750 共享总线）

## 硬件清单与引脚

| 模块 | 接口 | GPIO |
|---|---|---|
| DHT22 温湿度 | 单总线 | GPIO 4 |
| PIR 人体检测 | 数字输入 | GPIO 5 |
| BH1750 光照 | I²C (SDA/SCL) | GPIO 21 / 22 |
| SSD1306 OLED | I²C (共享总线) | GPIO 21 / 22 |
| MQ-135 CO₂ | ADC | GPIO 34 |
| MQ-2 烟雾 | ADC | GPIO 35 |
| 蜂鸣器 | 数字输出（低电平触发） | GPIO 18 |
| LED（灯/窗帘/空调） | 数字输出 | GPIO 32 / 33 / 25 |
| ESP32-CAM | UART2 (TX/RX) | GPIO 16 / 17 |
| LD3320 语音 | SPI (SCK/MISO/MOSI/CS) | GPIO 14 / 12 / 13 / 15 |
| LD3320 RST / IRQ | 数字 | GPIO 2 / 27 |

> 详细引脚定义见 [main/app_core.h](main/app_core.h)。

## 构建与烧录

要求：ESP-IDF **5.5.4**（含 Xtensa GCC 工具链）、Python 3.11。

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

或使用本仓库脚本（Windows，路径可通过环境变量 `IDF_PATH` / `IDF_PYTHON_ENV_PATH` / `IDF_TOOLS_PATH` 覆盖）：

- `build_s7.bat` — 构建
- `rebuild.bat` — 重新构建
- `flash_s7.bat [COM口]` — 烧录（默认 COM6）
- `monitor.py [COM口]` — 串口监视（默认 COM6，需 `pyserial`）

关键配置（flash 2MB、DIO 40MHz、单分区表）见 [sdkconfig.defaults](sdkconfig.defaults)，会自动应用到首次构建。

## 配置说明

设备首次启动会把默认配置写入 NVS（见 [app_wifi.c](main/app_wifi.c)）：

- **SoftAP**：SSID `SmartHome`，默认密码 `changeme`（占位符，请通过 NVS 覆盖为真实密码）
- **MQTT Broker**：`192.168.4.2:1883`（本地内网地址）
- 修改后如需恢复默认值，可擦除 NVS 分区：

```bash
idf.py erase-flash
```

## 目录结构

```
main/
  main.c          入口，任务创建与初始化
  app_core.h      全局引脚/配置/数据结构
  app_wifi.c      SoftAP 启动 + NVS 配置读写
  app_sensor.c    传感器采集任务（DHT22/BH1750/MQ-x/PIR）
  app_mqtt.c      MQTT 任务（SNTP 校时 + 数据上报 + 指令下发）
  app_display.c   OLED 显示任务
  app_ssd1306.c   SSD1306 驱动
  app_ld3320.c    LD3320 语音识别
  app_web.c       HTTP Web 面板 + SSE 状态推送
  app_camera.c    ESP32-CAM 串口接入
```

## License

[MIT](LICENSE)
