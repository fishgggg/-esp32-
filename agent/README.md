# SmartHome Agent — 物联网数据判定 Agent

> ⚠️ **本仓库当前为测试版本（Test Version）**
> - LLM 判定为 **mock 实现**（不发网络请求，用启发式模拟）
> - 通知渠道为 **console mock**（打印到控制台，不推微信）
> - 阈值均为**占位假定值**，需实测标定
> - 密钥已全部脱敏

基于 [fishgggg/-esp32-](https://github.com/fishgggg/-esp32-) 的补充 Agent：订阅 ESP32 上报的 `smarthome/sensor` 数据流，做分级判定（规则为主 + LLM 可选），主动推送告警与报告。

## 设计文档

- [DESIGN.md](DESIGN.md) — 整体设计
- [MVP-PLAN.md](MVP-PLAN.md) — MVP 实现方案（含真实 JSON 结构）

## 功能

- 订阅 `smarthome/sensor`，解析 ESP32 上报 JSON（`temp/humi/lux/co2/pm25/pir`）
- 规则引擎：三级状态机（normal→warning→critical）+ 迟滞 + 冷却
- 分级告警：warning→LLM 软分析，critical→硬告警，超时→设备离线硬告警
- 通知：console（测试版）｜Server酱（真实模式）
- SQLite 存储采样与告警历史

## 快速开始（测试版，零外网依赖）

```bash
pip install pyyaml        # 唯一必需依赖

# 模拟演示：走完「正常→软分析→硬告警→恢复→离线」全链路
python -m smarthome_agent.demo
```

输出示例：

```
--- ② CO₂ 越过 warning(900) → 软分析 ---
========================================================
📤 通知（console mock）
标题: [SmartHome告警] CO₂ 偏高
内容:
## CO₂ 偏高
- 时间：...
- 当前值：930（阈值 900）
- 判定：正常波动（置信度 0.85）
- 说明：CO₂轻微越过阈值，属正常波动
- 建议：继续观察即可
========================================================
```

## 真实模式（后续启用）

```bash
pip install -r requirements.txt        # 含 paho-mqtt / requests

# 1) 改 config.yaml：llm.mode=real、notify.mode=serverchan、填 sendkey
# 2) 启动真实 MQTT 模式
python -m smarthome_agent.main config.yaml
```

## 目录结构

```
smarthome_agent/
  main.py         # 真实 MQTT 入口
  demo.py         # 模拟演示入口（测试版）
  pipeline.py     # 事件 -> 文案 -> 通知 -> 落库
  rule_engine.py  # 状态机 + 迟滞 + 冷却 + 离线
  llm_judge.py    # LLM 判定（mock / real）
  notifier.py     # 通知渠道（console / Server酱）
  storage.py      # SQLite
  mqtt_client.py  # MQTT 订阅
  models.py       # 数据结构
  config.py       # 配置加载
  logger.py       # 日志
config.yaml       # 全部可调参数
```

## 阈值标定说明

`config.yaml` 里的 `rules.sensors` 阈值（co2=900/1200、pm25=1200/1500、temp=30/35）为**假定值**。正式使用前，让 ESP32 在正常环境跑一段时间，观察基线（参考：CO₂ 基线~784、烟雾基线~975），再据此设定 warning/critical。
