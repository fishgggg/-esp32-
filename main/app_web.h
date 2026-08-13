/**
 * @file    app_web.h
 * @brief   SmartHome Web 仪表盘 — HTTP 服务器接口 (S7)
 */

#ifndef APP_WEB_H
#define APP_WEB_H

#include "app_core.h"

/**
 * @brief 启动 HTTP 服务器任务
 * @param pvParameters  HttpTaskParams* (sensor_queue + cmd_queue)
 */
void StartHttpTask(void *pvParameters);

#endif /* APP_WEB_H */
