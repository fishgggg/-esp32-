/**
 * @file    app_sensor.h
 * @brief   SensorTask — 传感器采集任务 (S1 空壳)
 */

#ifndef APP_SENSOR_H
#define APP_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SensorTask 入口 (Core 0, 周期 2s)
 *
 * S2 将实现: DHT22(RMT) + BH1750(I²C) + MQ-135/MQ-2(ADC) + PIR(GPIO)
 *             → 阈值判断 → 蜂鸣器控制
 */
void StartSensorTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H */
