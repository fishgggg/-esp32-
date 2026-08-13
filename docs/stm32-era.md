# STM32 阶段（背景与缘起）

> 这是项目的起点。当前已放弃，仅作背景记录。以下内容来自当时的开发记忆。

## 硬件架构

- **主控**：STM32F103C8T6（Cortex-M3，64K Flash / 20K RAM）
- **外设**：GPIO / USART / ADC / TIM，RTOS 用 FreeRTOS（CMSIS-RTOS v2）
- **联网**：外接 ESP32 做 WiFi 网卡，走 **AT 指令**桥接（`AT+CWJAP` 等）；ESP32 收到的 TCP 数据以 `+IPD` 形式经串口进入 STM32
- **领域术语**：Sensor（传感器）/ Relay（继电器）/ Zone（房间区域）/ Threshold（告警阈值）

## 无法根治的两个 bug（放弃的直接原因）

### 1. `osDelay(N≥214)` 必 HardFault —— 阈值精确锁定

- **现象**：MQTTTask 中调用 `osDelay(N)`，**N≤212 正常，N≥214 HardFault**，边界仅差 2 ticks（2ms）
- **排查过程**：
  - 二分法定位：内存损坏发生在 tick 214~216 之间
  - 完整审查 FreeRTOS v10.3.1 内核（vTaskDelay / xTaskIncrementTick / 延迟链表）——逻辑全部正确
  - 排除 Timer 任务干扰（优先级极低）
- **推测根因**：MQTTTask 阻塞期间，其 TCB 或栈被其他代码损坏，唤醒瞬间崩溃
- **Workaround**：`osDelay(3000)` → `for (i<3000) osDelay(1)` 拆小延时

### 2. USART 无硬件 FIFO，接收溢出丢数据

- **现象**：STM32F103 USART 只有 1 字节 DR 寄存器，无 FIFO。MQTTTask 在 `osDelay(100)` 阻塞期间，ESP32 连续发 ~55 字节 `+IPD`，STM32 只捕获 1-2 字节，其余 ORE（Overrun Error）丢弃
- **修复**：ISR + **2048B 环形缓冲区**（SPSC 无锁设计：ISR 只写 head，主循环只读 tail），主循环阻塞改为 `osDelay(10)`
- **注意**：STM32F1 HAL 宏前缀用 `__HAL_UART_*`，不是 `__HAL_USART_*`；RXNE / ORE 标志同理

## 为什么放弃 STM32

1. **资源极其紧张**：64K Flash / 20K RAM，跑 FreeRTOS + WiFi 桥接 + MQTT 已到极限
2. **问题纠缠内核**：osDelay HardFault 与 FreeRTOS 运行状态相关，无法根治
3. **ESP32 自带 WiFi** + 双核大内存，从根上消除这两类问题

→ 迁移决策见 [esp32-era/00-migration.md](esp32-era/00-migration.md)
