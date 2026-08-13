/**
 * @file    app_core.h
 * @brief   SmartHome ESP32 — 全局引脚定义、数据结构、配置常量
 *
 * 引脚映射表 (ESP32-DevKit-32E):
 *   DHT22:        GPIO 4   | PIR:          GPIO 5
 *   Buzzer:       GPIO 18  | I2C SDA/SCL:  GPIO 21/22
 *   MQ-135 ADC:   GPIO 34  | MQ-2 ADC:     GPIO 35
 *   UART2→CAM:    GPIO 16(TX) / 17(RX)
 *   LD3320 SPI:   GPIO 14(SCK) 12(MISO) 13(MOSI) 15(CS)
 *   LD3320 RST/IRQ: GPIO 2 / 27
 */

#ifndef APP_CORE_H
#define APP_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/gpio_types.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 引脚定义
 * ================================================================ */

/* --- 传感器 --- */
#define DHT22_GPIO          GPIO_NUM_4
#define PIR_GPIO            GPIO_NUM_5

/* --- 执行器 --- */
#define BUZZER_GPIO         GPIO_NUM_18   /* 低电平触发 */
#define RELAY1_GPIO         GPIO_NUM_32   /* 预留，硬件未接 */
#define RELAY2_GPIO         GPIO_NUM_33   /* 预留，硬件未接 */
#define RELAY3_GPIO         GPIO_NUM_25   /* 预留，硬件未接 */
#define RELAY4_GPIO         GPIO_NUM_26   /* 预留，硬件未接 */

/* S8: 设备状态 LED 别名 (100Ω~220Ω 限流电阻 → LED → GND, 高电平=亮) */
#define LIGHT_LED_GPIO      RELAY1_GPIO   /* GPIO32 — 灯 */
#define CURTAIN_LED_GPIO    RELAY2_GPIO   /* GPIO33 — 窗帘 */
#define AC_LED_GPIO         RELAY3_GPIO   /* GPIO25 — 空调 */

/* 蜂鸣器打盹时长 (用户手动关闭后, 3s 内不会因超标再次触发) */
#define BUZZER_SNOOZE_MS    3000

/* --- I²C 总线 (BH1750 + SSD1306 共享) --- */
#define I2C_MASTER_SCL_IO   GPIO_NUM_22
#define I2C_MASTER_SDA_IO   GPIO_NUM_21
#define I2C_MASTER_FREQ_HZ  100000       /* 标准模式 100KHz */
#define I2C_MASTER_NUM      0            /* I2C0 */

#define BH1750_ADDR         0x23
#define SSD1306_ADDR        0x3C

/* --- ADC (MQ-135 空气 + MQ-2 烟雾) --- */
#define ADC_MQ135_CHANNEL   ADC_CHANNEL_6   /* GPIO 34 */
#define ADC_MQ2_CHANNEL     ADC_CHANNEL_7   /* GPIO 35 */
#define ADC_ATTEN           ADC_ATTEN_DB_0   /* 0-1.1V (MQ 传感器输出 ≤1V, 最灵敏档) */
#define ADC_WIDTH           ADC_BITWIDTH_12  /* 12-bit, 0-4095 */

/* --- UART → ESP32-CAM --- */
#define CAM_UART_NUM        2             /* UART2 */
#define CAM_UART_TX         GPIO_NUM_16
#define CAM_UART_RX         GPIO_NUM_17
#define CAM_UART_BAUD       115200

/* --- LD3320 语音识别 (VSPI) --- */
#define LD3320_SPI_HOST     SPI2_HOST
#define LD3320_SCK_IO       GPIO_NUM_14
#define LD3320_MISO_IO      GPIO_NUM_12
#define LD3320_MOSI_IO      GPIO_NUM_13
#define LD3320_CS_IO        GPIO_NUM_15
#define LD3320_RST_IO       GPIO_NUM_2
#define LD3320_IRQ_IO       GPIO_NUM_27

/* ================================================================
 * 数据结构
 * ================================================================ */

typedef struct {
    float    temperature;   /* ℃ */
    float    humidity;      /* %  */
    uint16_t light;         /* Lux */
    uint16_t co2;           /* MQ-135 → CO₂ 估算 ppm */
    uint16_t pm25;          /* MQ-2 → 烟雾 0-1000 */
    bool     motion;        /* PIR 人体检测 */
} SensorData;

typedef enum {
    CMD_BUZZER = 0,
    CMD_RELAY,
    CMD_CURTAIN,
} CmdType;

typedef struct {
    CmdType  type;
    uint8_t  id;            /* 子设备 ID (0=relay1, 1=relay2, ...) */
    int16_t  value;         /* 主参数 (on/off, position, ...) */
    int16_t  sub_value;     /* 附加参数 (duration ms, ...) */
} ControlCmd;

/* S8: 设备状态 (由 SensorTask/HttpTask 维护, SSE 每 2s 推送) */
typedef struct {
    bool     buzzer;        /* 蜂鸣器: true=响, false=关 */
    bool     light;         /* 灯:    true=开, false=关 (GPIO32 LED) */
    bool     curtain;       /* 窗帘:  true=开, false=关 (GPIO33 LED) */
    bool     ac;            /* 空调:  true=开, false=关 (GPIO25 LED) */
    int64_t  buzzer_snooze_until;  /* 蜂鸣器打盹到期 tick (0=不打盹) */
} DeviceState;

/* ================================================================
 * 告警阈值
 * ================================================================ */

#define SMOKE_THRESHOLD     1500   /* MQ-2 PM25 阈值, 基线~975, 实测校准 */
#define CO2_THRESHOLD       1200   /* MQ-135 CO₂ 阈值, 基线~784, 实测校准 */

/* ================================================================
 * 默认配置 (NVS 回退值)
 * ================================================================ */

#define DEFAULT_AP_SSID         "SmartHome"
#define DEFAULT_AP_PASSWORD     "changeme"   /* 公开仓库: 默认值已脱敏, 实际密码经 NVS 覆盖 (见 app_wifi.c) */
#define DEFAULT_MQTT_BROKER_IP  "192.168.4.2"
#define DEFAULT_MQTT_PORT       1883
#define DEFAULT_MQTT_CLIENT_ID  "esp32_smarthome"

#define MQTT_TOPIC_SENSOR       "smarthome/sensor"
#define MQTT_TOPIC_CMD          "smarthome/cmd"

/* ================================================================
 * 采集周期 (ms)
 * ================================================================ */

#define SENSOR_PERIOD_MS    2000

/* ================================================================
 * 全局 I²C 总线句柄 (main.c 初始化, SensorTask/DisplayTask 共用)
 * 前向声明避免引入重型头文件
 * ================================================================ */
struct i2c_master_bus_t;
extern struct i2c_master_bus_t *g_i2c_bus;

/**
 * @brief SensorTask 参数 (从 main.c 传入)
 */
typedef struct {
    void *mqtt_queue;       /* → MQTTTask   (QueueHandle_t) */
    void *display_queue;    /* → DisplayTask (QueueHandle_t) */
    void *web_queue;        /* → HttpTask   (QueueHandle_t), S7 fix: 独立队列避免饥饿 */
} SensorTaskParams;

/* ================================================================
 * Web/HTTP 任务配置 (S7)
 * ================================================================ */

#define HTTP_TASK_STACK     6144
#define HTTP_TASK_PRIO      3
#define HTTP_PORT           80

/**
 * @brief HttpTask 参数 (从 main.c 传入)
 */
typedef struct {
    void *sensor_queue;     /* ← 读取最新 SensorData */
    void *cmd_queue;        /* → 推送控制命令 */
} HttpTaskParams;

/* ---- 全局队列句柄 ---- */
extern void *g_sensor_queue;   /* SensorData 队列, HTTP 需要读取 */
extern void *g_cmd_queue;      /* ControlCmd 队列, 统一命令路由 */

/* ---- 全局设备状态 (S8) ---- */
extern DeviceState g_device_state;

#ifdef __cplusplus
}
#endif

#endif /* APP_CORE_H */
