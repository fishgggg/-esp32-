/**
 * @file    app_mqtt.h
 * @brief   MQTTTask — MQTT 通信任务 (S1 空壳)
 */

#ifndef APP_MQTT_H
#define APP_MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTTTask 入口 (Core 1)
 *
 * S4 将实现: esp_mqtt_client 初始化 → Broker 连接
 *            → PUBLISH smarthome/sensor (cJSON 序列化)
 *            → SUBSCRIBE smarthome/cmd (cJSON 解析 → controlQueue)
 */
void StartMQTTTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* APP_MQTT_H */
