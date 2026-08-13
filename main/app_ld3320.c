/**
 * @file    app_ld3320.c
 * @brief   LD3320 语音识别芯片 SPI 驱动 (S6)
 *
 * SPI 协议:
 *   写寄存器: CS↓ → 0x04 → 地址 → 数据 → CS↑
 *   读寄存器: CS↓ → 0x05 → 地址 → 读回数据 → CS↑
 *   CPOL=1 (CLK 空闲高), CPHA=0 (前沿采样) — Arduino SPI_MODE2
 *
 * 参考: LD3320 开发手册, Waveshare LD3320 Board
 */

#include "app_ld3320.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ld3320";

/* ================================================================
 * SPI 常量
 * ================================================================ */
#define LD_SPI_HOST     SPI2_HOST
#define LD_SPI_CLK_HZ      1000000     /* 1 MHz (安全) */

#define SPI_CMD_WRITE   0x04
#define SPI_CMD_READ    0x05

/* ================================================================
 * LD3320 关键寄存器
 * ================================================================ */
#define REG_FIFO_DATA       0x05    /* FIFO 数据端口 (写拼音) */
#define REG_FIFO_CTRL       0x08    /* FIFO 控制 (1=清FIFO, 4=清FIFO_EXT) */
#define REG_CLK_FREQ        0x11    /* PLL 频率设置 */
#define REG_RESET           0x17    /* 软复位: 0x35→复位, 0x4C→DSP休眠 */
#define REG_ADC_CTRL        0x1C    /* ADC 控制: 0x09=保留, 0x0B=开启MIC */
#define REG_CLK_FREQ2       0x19    /* PLL ASR 频率 2 */
#define REG_CLK_FREQ3       0x1B    /* PLL ASR 频率 3 */
#define REG_CLK_FREQ4       0x1D    /* PLL ASR 频率 4 */
#define REG_INT_EN          0x29    /* 中断使能: 0x10=ASR中断 */
#define REG_INT_SRC         0x2B    /* 中断源: bit4=ASR结果 */
#define REG_MIC_VOL         0x35    /* MIC 音量 (0x00-0x7F) */
#define REG_ASR_CMD         0x37    /* ASR 命令: 0x04=添加短语, 0x06=开始识别 */
#define REG_FIFO_EXT_START  0x3C    /* FIFO_EXT 低字节起始地址 */
#define REG_FIFO_EXT_END    0x3E    /* FIFO_EXT 高字节结束地址 */
#define REG_FIFO_START      0x38    /* FIFO 低字节起始 */
#define REG_FIFO_END        0x3A    /* FIFO 高字节结束 */
#define REG_FIFO_EXT_START2 0x40    /* 另一组 FIFO_EXT 起始 */
#define REG_FIFO_EXT_END2   0x42    /* 另一组 FIFO_EXT 结束 */
#define REG_FIFO_START2     0x44    /* 另一组 FIFO 起始 */
#define REG_FIFO_END2       0x46    /* 另一组 FIFO 结束 */
#define REG_POWER_SAVE      0x89    /* 模拟电路控制 */
#define REG_DSP_STATUS      0xB2    /* DSP 状态: 0x21=空闲 */
#define REG_VAD_PARAM       0xB3    /* VAD 灵敏度 (1-80) */
#define REG_VAD_START       0xB4    /* VAD 开始时间 (10ms/unit) */
#define REG_VAD_END         0xB5    /* VAD 结束静音 (10ms/unit) */
#define REG_VAD_MAX         0xB6    /* VAD 最大时长 (100ms/unit) */
#define REG_PASS_FRAME      0xB7    /* 跳帧数 (20ms/frame) */
#define REG_STRING_LEN      0xB9    /* 拼音字符串长度 */
#define REG_RESULT_COUNT    0xBA    /* 候选结果数 (1-4) */
#define REG_ASR_CTRL        0xBD    /* ASR 控制: 0x00=启动, 0x20=保留 */
#define REG_KW_ID           0xC1    /* 当前关键词 ID (写入) */
#define REG_KW_DATA         0xC3    /* 关键词数据 (写0x00) */
#define REG_RESULT_BEST     0xC5    /* 最佳匹配结果 ID (读取) */
#define REG_CFG_DSP         0xCD    /* DSP 睡眠设置 */
#define REG_POWER           0xCF    /* 省电模式 */

/* PLL 值 (22.1184 MHz 晶振 → ASR 模式)
 * 公式: LD_PLL_11 = CLK_IN/2 - 1
 *       LD_PLL_ASR_19 = CLK_IN * 32 / (LD_PLL_11 + 1) - 0.51
 *       LD_PLL_ASR_1B = 0x48 (固定)
 *       LD_PLL_ASR_1D = 0x1F (固定)
 */
#define LD_CLK_IN           22.1184f
#define PLL_11              ((uint8_t)(LD_CLK_IN / 2.0f - 1.0f))             /* 0x0A */
#define PLL_ASR_19          ((uint8_t)(LD_CLK_IN * 32.0f / (PLL_11 + 1) - 0.51f)) /* 0x3F */
#define PLL_ASR_1B          0x48
#define PLL_ASR_1D          0x1F

/* VAD 参数 */
#define MIC_VOL             0x43    /* 麦克风音量 */
#define VAD_SENSITIVITY     0x12    /* VAD 灵敏度 */
#define VAD_START_MS        0x0F    /* 语音开始 150ms */
#define VAD_END_MS          0x3C    /* 静音结束 600ms */
#define VAD_MAX_MS          0x3C    /* 最大时长 6s */
#define PASS_FRAMES         0x02    /* 跳 2 帧 = 40ms */

/* ================================================================
 * SPI 设备句柄
 * ================================================================ */
static spi_device_handle_t spi_dev = NULL;

/* ================================================================
 * 关键词列表 (本地缓存)
 * ================================================================ */
static LD3320Keyword kw_list[LD3320_MAX_KEYWORDS];
static int kw_count = 0;

/* ================================================================
 * SPI 读写底层
 * ================================================================ */

static void spi_write_reg(uint8_t addr, uint8_t data)
{
    spi_transaction_t t = {
        .flags     = SPI_TRANS_USE_TXDATA,
        .cmd       = SPI_CMD_WRITE,
        .addr      = addr,
        .length    = 16,               /* cmd(8) + addr(8) */
        .tx_data   = { data, 0, 0, 0 },
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static uint8_t spi_read_reg(uint8_t addr)
{
    spi_transaction_t t = {
        .flags     = SPI_TRANS_USE_RXDATA,
        .cmd       = SPI_CMD_READ,
        .addr      = addr,
        .length    = 24,               /* cmd(8) + addr(8) + dummy(8) */
        .rxlength  = 8,
    };
    spi_device_polling_transmit(spi_dev, &t);
    return t.rx_data[0];
}

/* ================================================================
 * 芯片复位 (RST 引脚: 低→延时→高)
 * ================================================================ */
static void ld3320_hw_reset(void)
{
    gpio_set_level(LD3320_RST_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(LD3320_RST_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Hardware reset done");
}

/* ================================================================
 * 等待 DSP 空闲 (检查 0xB2 == 0x21, 超时 100ms)
 * ================================================================ */
static int ld3320_wait_idle(void)
{
    for (int i = 0; i < 20; i++) {
        uint8_t status = spi_read_reg(REG_DSP_STATUS);
        if (status == 0x21 || status == 0xFF) {
            ESP_LOGI(TAG, "wait_idle OK: DSP_STATUS=0x%02X (after %d tries)", status, i);
            return 1;
        }
        if (i == 0 || i == 19) {
            ESP_LOGI(TAG, "wait_idle[%d]: DSP_STATUS=0x%02X", i, status);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

/* ================================================================
 * 通用初始化 (复位 + 时钟 + 模拟电路)
 * ================================================================ */
static void ld3320_init_common(void)
{
    spi_read_reg(0x06);     /* 空读一次, 稳定 SPI */

    spi_write_reg(REG_RESET, 0x35);     /* 软复位 */
    vTaskDelay(pdMS_TO_TICKS(10));
    spi_read_reg(0x06);

    spi_write_reg(REG_POWER_SAVE, 0x03);    /* 模拟电路 */
    vTaskDelay(pdMS_TO_TICKS(5));
    spi_write_reg(REG_POWER, 0x43);         /* 省电设置 */
    vTaskDelay(pdMS_TO_TICKS(5));
    spi_write_reg(0xCB, 0x02);

    /* PLL 时钟配置 (22.1184 MHz 晶振) */
    spi_write_reg(REG_CLK_FREQ,  PLL_11);
    spi_write_reg(0x1E,          0x00);
    spi_write_reg(REG_CLK_FREQ2, PLL_ASR_19);
    spi_write_reg(REG_CLK_FREQ3, PLL_ASR_1B);
    spi_write_reg(REG_CLK_FREQ4, PLL_ASR_1D);

    vTaskDelay(pdMS_TO_TICKS(10));

    spi_write_reg(REG_CFG_DSP, 0x04);       /* DSP 睡眠 */
    spi_write_reg(REG_RESET,  0x4C);        /* DSP 睡眠模式 */
    vTaskDelay(pdMS_TO_TICKS(5));
    spi_write_reg(REG_STRING_LEN, 0x00);
    spi_write_reg(REG_POWER,     0x4F);
    spi_write_reg(0x6F,          0xFF);

    ESP_LOGI(TAG, "Common init done");
}

/* ================================================================
 * ASR 模式初始化 (DSP 激活 + FIFO 配置)
 * ================================================================ */
static void ld3320_init_asr(void)
{
    ld3320_init_common();

    spi_write_reg(REG_ASR_CTRL, 0x00);
    spi_write_reg(REG_RESET,    0x48);      /* 激活 DSP */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* FIFO_EXT 边界 */
    spi_write_reg(REG_FIFO_EXT_START,  0x80);
    spi_write_reg(REG_FIFO_EXT_END,    0x07);
    spi_write_reg(REG_FIFO_START,      0xFF);
    spi_write_reg(REG_FIFO_END,        0x07);
    spi_write_reg(REG_FIFO_EXT_START2, 0x00);
    spi_write_reg(REG_FIFO_EXT_END2,   0x08);
    spi_write_reg(REG_FIFO_START2,     0x00);
    spi_write_reg(REG_FIFO_END2,       0x08);
    vTaskDelay(pdMS_TO_TICKS(1));

    ESP_LOGI(TAG, "ASR init done");
}

/* ================================================================
 * 公共 API
 * ================================================================ */

esp_err_t LD3320_Init(void)
{
    ESP_LOGI(TAG, "Initializing LD3320...");

    /* ---- RST / IRQ / CS GPIO 配置 ---- */
    gpio_set_direction(LD3320_RST_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(LD3320_RST_IO, 1);           /* RST 初始高 */

    gpio_set_direction(LD3320_IRQ_IO, GPIO_MODE_INPUT);
    /* 不配置上拉 — 模块自带 */

    gpio_set_direction(LD3320_CS_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(LD3320_CS_IO, 1);            /* CS 初始高 */

    /* ---- VSPI 总线初始化 ---- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LD3320_MOSI_IO,
        .miso_io_num     = LD3320_MISO_IO,
        .sclk_io_num     = LD3320_SCK_IO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 16,
    };
    esp_err_t ret = spi_bus_initialize(LD_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- SPI 设备配置 ---- */
    spi_device_interface_config_t dev_cfg = {
        .mode           = 2,                    /* CPOL=1, CPHA=0 */
        .clock_speed_hz = LD_SPI_CLK_HZ,
        .spics_io_num   = LD3320_CS_IO,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    ret = spi_bus_add_device(LD_SPI_HOST, &dev_cfg, &spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "VSPI ready (SCK=%d MISO=%d MOSI=%d CS=%d)",
             LD3320_SCK_IO, LD3320_MISO_IO, LD3320_MOSI_IO, LD3320_CS_IO);

    /* ---- 硬件复位 → ASR 初始化 ---- */
    ld3320_hw_reset();
    ld3320_init_asr();

    ESP_LOGI(TAG, "LD3320 initialized OK");
    return ESP_OK;
}

int LD3320_AddKeyword(const LD3320Keyword *kw)
{
    if (kw_count >= LD3320_MAX_KEYWORDS) {
        ESP_LOGE(TAG, "Keyword list full");
        return -1;
    }
    if (kw == NULL || kw->pinyin[0] == '\0') {
        return -1;
    }

    /* 复制到本地列表 */
    kw_list[kw_count] = *kw;

    /* ---- 等待 DSP 空闲 ---- */
    if (!ld3320_wait_idle()) {
        ESP_LOGE(TAG, "DSP timeout — cannot add keyword '%s'", kw->pinyin);
        return -1;
    }

    /* ---- 写入关键词到芯片 ---- */
    spi_write_reg(REG_KW_ID,   kw->id);
    spi_write_reg(REG_KW_DATA, 0x00);

    spi_write_reg(REG_FIFO_CTRL, 0x04);     /* 清 FIFO_EXT */
    vTaskDelay(pdMS_TO_TICKS(1));
    spi_write_reg(REG_FIFO_CTRL, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* 逐字符写入拼音 */
    int len = strlen(kw->pinyin);
    for (int i = 0; i < len; i++) {
        spi_write_reg(REG_FIFO_DATA, (uint8_t)kw->pinyin[i]);
    }

    spi_write_reg(REG_STRING_LEN, (uint8_t)len);
    spi_write_reg(REG_DSP_STATUS, 0xFF);
    spi_write_reg(REG_ASR_CMD,    0x04);     /* 添加短语命令 */

    ESP_LOGI(TAG, "Keyword #%d added: '%s' (%d chars)", kw->id, kw->pinyin, len);
    kw_count++;
    return 0;
}

void LD3320_Start(void)
{
    if (kw_count == 0) {
        ESP_LOGW(TAG, "No keywords added — skip start");
        return;
    }

    /* VAD + MIC 配置 */
    spi_write_reg(REG_MIC_VOL,    MIC_VOL);
    spi_write_reg(REG_VAD_PARAM,  VAD_SENSITIVITY);
    spi_write_reg(REG_VAD_START,  VAD_START_MS);
    spi_write_reg(REG_VAD_END,    VAD_END_MS);
    spi_write_reg(REG_VAD_MAX,    VAD_MAX_MS);
    spi_write_reg(REG_PASS_FRAME, PASS_FRAMES);

    spi_write_reg(REG_ADC_CTRL, 0x09);      /* ADC 保留 */
    spi_write_reg(REG_ASR_CTRL, 0x20);      /* 保留 */
    spi_write_reg(REG_FIFO_CTRL, 0x01);     /* 清 FIFO */
    vTaskDelay(pdMS_TO_TICKS(1));
    spi_write_reg(REG_FIFO_CTRL, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (!ld3320_wait_idle()) {
        ESP_LOGW(TAG, "DSP not idle before start");
    }

    spi_write_reg(REG_DSP_STATUS, 0xFF);
    spi_write_reg(REG_ASR_CMD,    0x06);     /* 开始识别命令 */
    vTaskDelay(pdMS_TO_TICKS(5));
    spi_write_reg(REG_ADC_CTRL, 0x0B);      /* 开启 MIC */
    spi_write_reg(REG_INT_EN,   0x10);      /* 使能 ASR 中断 */
    spi_write_reg(REG_ASR_CTRL, 0x00);      /* 启动 ASR */

    ESP_LOGI(TAG, "Recognition started — listening...");
}

int LD3320_GetResult(void)
{
    uint8_t irq = spi_read_reg(REG_INT_SRC);

    if (irq & 0x10) {                       /* bit4 = ASR 结果就绪 */
        spi_write_reg(REG_INT_EN, 0x00);    /* 关中断 */
        spi_write_reg(0x02, 0x00);

        uint8_t count = spi_read_reg(REG_RESULT_COUNT);
        if (count > 0) {
            uint8_t best = spi_read_reg(REG_RESULT_BEST);
            ESP_LOGI(TAG, "Voice result: id=%d (candidates=%d)", best, count);
            return (int)best;
        }
    }
    return -1;                              /* 无结果 */
}

/* ================================================================
 * VoiceTask — 语音识别任务 (Core 1, prio 3)
 * ================================================================ */

#define VOICE_TASK_PERIOD_MS    100     /* IRQ 轮询间隔 */

void StartVoiceTask(void *pvParameters)
{
    ESP_LOGI(TAG, "VoiceTask starting on Core %d", xPortGetCoreID());

    /* ---- 1. 初始化 LD3320 ---- */
    if (LD3320_Init() != ESP_OK) {
        ESP_LOGE(TAG, "LD3320 init failed — VoiceTask abort");
        vTaskDelete(NULL);
        return;
    }

    /* ---- 2. 添加关键词 ---- */
    LD3320Keyword keywords[] = {
        { .id = 0, .pinyin = "kai deng"   },   /* 开灯 */
        { .id = 1, .pinyin = "guan deng"  },   /* 关灯 */
        { .id = 2, .pinyin = "bao jing"   },   /* 报警 */
        { .id = 3, .pinyin = "ting zhi"   },   /* 停止 */
        { .id = 4, .pinyin = "da kai feng shan" }, /* 打开风扇 */
        { .id = 5, .pinyin = "guan bi feng shan" }, /* 关闭风扇 */
    };
    int kw_n = sizeof(keywords) / sizeof(keywords[0]);

    for (int i = 0; i < kw_n; i++) {
        if (LD3320_AddKeyword(&keywords[i]) != 0) {
            ESP_LOGW(TAG, "Failed to add keyword '%s'", keywords[i].pinyin);
        }
    }

    /* ---- 3. 开始识别 ---- */
    LD3320_Start();

    /* ---- 4. 主循环: 轮询 IRQ 引脚 ---- */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(VOICE_TASK_PERIOD_MS));

        /* IRQ 低有效 */
        if (gpio_get_level(LD3320_IRQ_IO) == 0) {
            int result = LD3320_GetResult();
            if (result >= 0) {
                /* ---- 匹配关键词 → 执行动作 ---- */
                switch (result) {
                case 0:   /* 开灯 */
                    ESP_LOGI(TAG, ">> VOICE: 开灯 (kai deng)");
                    break;
                case 1:   /* 关灯 */
                    ESP_LOGI(TAG, ">> VOICE: 关灯 (guan deng)");
                    break;
                case 2:   /* 报警 — 蜂鸣器响 3 秒 */
                    ESP_LOGI(TAG, ">> VOICE: 报警 (bao jing) — buzzer 3s");
                    gpio_set_level(BUZZER_GPIO, 0);   /* 响 */
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    gpio_set_level(BUZZER_GPIO, 1);   /* 关 */
                    break;
                case 3:   /* 停止 — 关蜂鸣器 */
                    ESP_LOGI(TAG, ">> VOICE: 停止 (ting zhi) — buzzer off");
                    gpio_set_level(BUZZER_GPIO, 1);
                    break;
                case 4:   /* 打开风扇 */
                    ESP_LOGI(TAG, ">> VOICE: 打开风扇 (da kai feng shan)");
                    break;
                case 5:   /* 关闭风扇 */
                    ESP_LOGI(TAG, ">> VOICE: 关闭风扇 (guan bi feng shan)");
                    break;
                default:
                    ESP_LOGI(TAG, ">> VOICE: unknown id=%d", result);
                    break;
                }

                /* 重新开始识别 */
                LD3320_Start();
            }
        }
    }
}
