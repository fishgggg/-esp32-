/**
 * @file    app_sensor.c
 * @brief   SensorTask — 传感器采集任务 (S2: 全传感器驱动)
 *
 * 每 2s 采集一轮:
 *   DHT22 (bit-bang GPIO) → 温湿度
 *   BH1750 (I²C)          → 光照 Lux
 *   MQ-135 (ADC1_CH6)     → CO₂ 估算 ppm
 *   MQ-2   (ADC1_CH7)     → 烟雾 0-1000
 *   PIR    (GPIO 输入)     → 人体检测 bool
 *
 * 所有模拟量通过 10 阶滑动平均滤波器平滑。
 */

#include "app_sensor.h"
#include "app_core.h"

#include "esp_log.h"
#include "esp_rom_sys.h"       /* esp_rom_delay_us */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "sensor";

/* ================================================================
 * 滑动平均滤波器 (10 阶)
 * ================================================================ */
#define FILTER_N 10

typedef struct {
    float   buf[FILTER_N];
    uint8_t idx;
    uint8_t count;
} Filter;

static float filter_update(Filter *f, float val)
{
    f->buf[f->idx] = val;
    f->idx = (f->idx + 1) % FILTER_N;
    if (f->count < FILTER_N) f->count++;
    float sum = 0;
    for (uint8_t i = 0; i < f->count; i++) sum += f->buf[i];
    return sum / f->count;
}

/* ================================================================
 * DHT22 驱动 (bit-bang, esp_rom_delay_us 微秒延时)
 *
 * 协议: 主机发起始信号 → DHT22 应答 80μs 低 + 80μs 高
 *       → 40 位数据 (湿度高8 + 湿度低8 + 温度高8 + 温度低8 + 校验)
 *       → bit '0' = 26-28μs 高电平, bit '1' = 70μs 高电平
 * ================================================================ */
static int dht22_read(float *temp, float *humi)
{
    uint8_t data[5] = {0};
    int timeout;

    /* ---- Step 1: 主机起始信号 (低 1ms, 高 30μs) ---- */
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_GPIO, 0);
    esp_rom_delay_us(1000);                     /* 拉低 1ms */
    gpio_set_level(DHT22_GPIO, 1);
    esp_rom_delay_us(30);                       /* 拉高 30μs */
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);

    /* ---- Step 2: 等待 DHT22 应答 ---- */
    /* 2a. 等待 DHT22 拉低 (80μs) */
    timeout = 1000;
    while (gpio_get_level(DHT22_GPIO) == 1) {
        if (--timeout == 0) return 1;           /* 超时 */
        esp_rom_delay_us(1);
    }
    /* 2b. 等待 DHT22 拉高 (80μs) */
    timeout = 1000;
    while (gpio_get_level(DHT22_GPIO) == 0) {
        if (--timeout == 0) return 1;
        esp_rom_delay_us(1);
    }
    /* 2c. 等待 DHT22 再次拉低 (数据起始) */
    timeout = 1000;
    while (gpio_get_level(DHT22_GPIO) == 1) {
        if (--timeout == 0) return 1;
        esp_rom_delay_us(1);
    }

    /* ---- Step 3: 读取 40 位数据 ---- */
    for (int i = 0; i < 5; i++) {
        for (int j = 7; j >= 0; j--) {
            /* 等待低电平结束 (每个 bit 以 50μs 低电平开始) */
            timeout = 500;
            while (gpio_get_level(DHT22_GPIO) == 0) {
                if (--timeout == 0) return 1;
                esp_rom_delay_us(1);
            }
            /* 在 40μs 处采样: 高电平还在 = bit '1', 已结束 = bit '0' */
            esp_rom_delay_us(40);
            if (gpio_get_level(DHT22_GPIO) == 1) {
                data[i] |= (1 << j);
            }
            /* 等待高电平结束 */
            timeout = 500;
            while (gpio_get_level(DHT22_GPIO) == 1) {
                if (--timeout == 0) return 1;
                esp_rom_delay_us(1);
            }
        }
    }

    /* ---- Step 4: 校验 ---- */
    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        return 2;                               /* 校验失败 */
    }

    *humi = (float)((data[0] << 8) | data[1]) / 10.0f;
    *temp = (float)(((data[2] & 0x7F) << 8) | data[3]) / 10.0f;
    if (data[2] & 0x80) *temp = -(*temp);      /* 负温度 */

    return 0;                                   /* 成功 */
}

/* ================================================================
 * BH1750 I²C 驱动
 *
 * 命令 0x10 = 连续高分辨率模式 (1lx, 典型 120ms/次)
 * 读数公式: Lux = raw / 1.2
 * ================================================================ */

static i2c_master_dev_handle_t bh1750_dev = NULL;

static esp_err_t bh1750_init(i2c_master_bus_handle_t bus)
{
    /* 添加 BH1750 设备到 I²C 总线 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_ADDR,
        .scl_speed_hz    = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &bh1750_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 发送连续高分辨率模式命令 (0x10) */
    uint8_t cmd = 0x10;
    ret = i2c_master_transmit(bh1750_dev, &cmd, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 init cmd failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t bh1750_read(uint16_t *lux)
{
    if (bh1750_dev == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t buf[2] = {0};
    esp_err_t ret = i2c_master_receive(bh1750_dev, buf, 2, 200);
    if (ret != ESP_OK) return ret;

    uint16_t raw = (buf[0] << 8) | buf[1];
    *lux = (uint16_t)(raw / 1.2f);
    return ESP_OK;
}

/* ================================================================
 * ADC Oneshot 驱动 — MQ-135 (CO₂) + MQ-2 (烟雾)
 *
 * 粗略估算公式 (清洁空气校准 ADC≈124 → CO₂≈494ppm, PM25≈91):
 *   CO₂(ppm) = 400 + adc_val * 3100 / 4095
 *   PM25     = adc_val * 3000 / 4095
 * ================================================================ */

static adc_oneshot_unit_handle_t adc1_handle = NULL;

static esp_err_t adc_init(void)
{
    /* 创建 ADC1 oneshot 单元 */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC oneshot new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置 MQ-135 通道 */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    ret = adc_oneshot_config_channel(adc1_handle, ADC_MQ135_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC config MQ-135 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置 MQ-2 通道 */
    ret = adc_oneshot_config_channel(adc1_handle, ADC_MQ2_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC config MQ-2 failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ================================================================
 * SensorTask 主循环
 * ================================================================ */
void StartSensorTask(void *pvParameters)
{
    SensorTaskParams *params = (SensorTaskParams *)pvParameters;
    QueueHandle_t mqtt_queue    = params ? params->mqtt_queue    : NULL;
    QueueHandle_t display_queue = params ? params->display_queue : NULL;
    QueueHandle_t web_queue     = params ? params->web_queue     : NULL;

    ESP_LOGI(TAG, "SensorTask starting on Core %d", xPortGetCoreID());

    /* ---- 滤波器初始化 ---- */
    Filter f_temp  = {0};
    Filter f_humi  = {0};
    Filter f_light = {0};
    Filter f_co2   = {0};
    Filter f_pm25  = {0};

    /* ---- BH1750 初始化 (使用 main.c 创建的 I²C 总线) ---- */
    esp_err_t bh1750_ret = bh1750_init(g_i2c_bus);
    if (bh1750_ret != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 init failed — will skip light readings");
    } else {
        ESP_LOGI(TAG, "BH1750 initialized (addr 0x%02X)", BH1750_ADDR);
    }

    /* ---- ADC 初始化 ---- */
    esp_err_t adc_ret = adc_init();
    if (adc_ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC init failed — will skip gas readings");
    } else {
        ESP_LOGI(TAG, "ADC1 oneshot ready (MQ-135 CH%d, MQ-2 CH%d)",
                 ADC_MQ135_CHANNEL, ADC_MQ2_CHANNEL);
    }

    /* ---- PIR GPIO 初始化 ---- */
    gpio_set_direction(PIR_GPIO, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "PIR GPIO%d configured as input", PIR_GPIO);

    /* ---- 蜂鸣器 GPIO 初始化 (低电平触发, 初始 HIGH=关) ---- */
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 1);  /* HIGH = 蜂鸣器关 */
    ESP_LOGI(TAG, "Buzzer GPIO%d initialized (active-low)", BUZZER_GPIO);

    /* ---- 告警状态 (S8: 支持打盹) ---- */
    bool alarm_active = false;

    /* ---- 传感器数据结构 (静态, 保留上次有效值) ---- */
    static SensorData data = {0};

    TickType_t last_wake = xTaskGetTickCount();
    ESP_LOGI(TAG, "SensorTask running — period %dms", SENSOR_PERIOD_MS);

    for (;;) {
        /* ==== DHT22 温湿度 ==== */
        float t, h;
        int dht_ret = dht22_read(&t, &h);
        if (dht_ret == 0) {
            data.temperature = filter_update(&f_temp, t);
            data.humidity    = filter_update(&f_humi, h);
        } else if (dht_ret == 1) {
            ESP_LOGW(TAG, "DHT22 timeout — keeping last values");
        } else {
            ESP_LOGW(TAG, "DHT22 checksum error — keeping last values");
        }

        /* ==== BH1750 光照 ==== */
        if (bh1750_dev != NULL) {
            uint16_t lux;
            if (bh1750_read(&lux) == ESP_OK) {
                data.light = (uint16_t)filter_update(&f_light, (float)lux);
            } else {
                ESP_LOGW(TAG, "BH1750 read failed");
            }
        }

        /* ==== MQ-135 (CO₂ 估算) ==== */
        int adc_mq135_raw = 0;
        if (adc1_handle != NULL) {
            int adc_val;
            if (adc_oneshot_read(adc1_handle, ADC_MQ135_CHANNEL, &adc_val) == ESP_OK) {
                adc_mq135_raw = adc_val;
                float co2 = 400.0f + (float)adc_val * 3100.0f / 4095.0f;
                data.co2 = (uint16_t)filter_update(&f_co2, co2);
            }
        }

        /* ==== MQ-2 (烟雾 PM2.5 估算) ==== */
        int adc_mq2_raw = 0;
        if (adc1_handle != NULL) {
            int adc_val;
            if (adc_oneshot_read(adc1_handle, ADC_MQ2_CHANNEL, &adc_val) == ESP_OK) {
                adc_mq2_raw = adc_val;
                float pm = (float)adc_val * 3000.0f / 4095.0f;
                data.pm25 = (uint16_t)filter_update(&f_pm25, pm);
            }
        }

        /* ==== PIR 人体检测 ==== */
        data.motion = (gpio_get_level(PIR_GPIO) == 1);

        /* ==== 日志输出 ==== */
        ESP_LOGI(TAG,
            "T=%.1f°C H=%.1f%% Lux=%u CO2=%uppm PM25=%u PIR=%s | ADC135=%d ADC2=%d",
            data.temperature, data.humidity,
            data.light, data.co2, data.pm25,
            data.motion ? "YES" : "no",
            adc_mq135_raw, adc_mq2_raw);

        /* ==== S3: 阈值判断 → 蜂鸣器告警 (S8: 支持 3s 打盹) ==== */
        bool danger = (data.pm25 > SMOKE_THRESHOLD) || (data.co2 > CO2_THRESHOLD);
        TickType_t now = xTaskGetTickCount();
        bool snoozing = (g_device_state.buzzer_snooze_until > 0)
                     && (now < (TickType_t)g_device_state.buzzer_snooze_until);

        if (danger && !alarm_active && !snoozing) {
            gpio_set_level(BUZZER_GPIO, 0);   /* 低电平 = 蜂鸣器响 */
            alarm_active = true;
            g_device_state.buzzer = true;
            ESP_LOGW(TAG, "⚠ ALARM ON (PM25=%u CO2=%u)", data.pm25, data.co2);
        } else if (!danger && alarm_active) {
            gpio_set_level(BUZZER_GPIO, 1);   /* 高电平 = 蜂鸣器关 */
            alarm_active = false;
            g_device_state.buzzer = false;
            g_device_state.buzzer_snooze_until = 0;
            ESP_LOGI(TAG, "✓ Alarm cleared");
        } else if (danger && snoozing) {
            ESP_LOGI(TAG, "⏳ Alarm snoozing (until tick %lld)", g_device_state.buzzer_snooze_until);
        }

        /* ==== S4/S5/S7: 发送 SensorData 到 MQTTTask + DisplayTask + HttpTask ==== */
        if (mqtt_queue != NULL) {
            xQueueSend(mqtt_queue, &data, 0);       /* 非阻塞, 队列满则丢弃 */
        }
        if (display_queue != NULL) {
            xQueueSend(display_queue, &data, 0);
        }
        if (web_queue != NULL) {
            xQueueSend(web_queue, &data, 0);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}
