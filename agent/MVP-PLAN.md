# 物联网数据判定 Agent — MVP 实现方案

> 状态：待实现｜基于真实 ESP32 代码（app_mqtt.c / app_core.h）反向对齐
> 前置文档：[DESIGN.md](DESIGN.md)

---

## 0. 与 DESIGN.md 的两处修正

读实际 ESP32 代码后，修正 DESIGN.md 里两处基于假设的写法：

1. **心跳包**：ESP32 无心跳字段，但每 2s 必发一条 `smarthome/sensor`。→ 设备离线判定改为"**超过 N 秒没收到任何消息**"，ESP32 零改动。
2. **字段名**：烟雾字段是 `pm25`（0-1000 估算值），不是 `smoke`。

---

## 1. 技术栈（MVP 全部用成熟库 / 标准库）

| 用途 | 选择 | 说明 |
|---|---|---|
| 语言 | Python 3.11+ | 与 ESP-IDF 无关，独立进程 |
| MQTT 客户端 | `paho-mqtt` | 订阅/解析/自动重连 |
| 配置 | `PyYAML` | 阈值/渠道/客户全外置 |
| LLM 调用 | `requests` POST（OpenAI 兼容端点） | qwen/doubao 都提供兼容接口，一个函数通吃，不装各家 SDK |
| 通知 | `requests`（Server酱 v3 HTTP） | `sctapi.ftqq.com` |
| 存储 | `sqlite3`（标准库） | 历史采样 + 告警记录 |
| 日志 | `logging`（标准库） | 结构化、分级 |

---

## 2. 目录结构

```
smarthome-agent/
  DESIGN.md
  MVP-PLAN.md
  requirements.txt        # paho-mqtt, PyYAML, requests
  config.yaml             # 全部可调参数（见 §6）
  smarthome_agent/
    __init__.py
    main.py               # 入口：装配各模块 + 启动事件循环
    config.py             # 加载/校验 YAML
    models.py             # 数据结构
    mqtt_client.py        # 订阅、解析、断线重连、离线检测
    rule_engine.py        # 规则引擎 + 状态机 + 迟滞 + 冷却
    llm_judge.py          # LLM 软分析（可开关）
    notifier.py           # 通知渠道（Server酱 / 邮件）
    storage.py            # SQLite 历史 + 告警
    logger.py             # 统一日志
```

---

## 3. 数据结构（models.py）

```python
@dataclass
class SensorSample:
    ts: str          # "2026-08-18T21:30:00"
    temp: float      # ℃
    humi: float      # %
    lux: int         # Lux
    co2: int         # CO₂ 估算 ppm
    pm25: int        # 烟雾 0-1000 估算
    pir: bool        # 人体检测

@dataclass
class Alert:
    level: str       # "hard"（规则直发）| "soft"（LLM 判定后发）
    kind: str        # offline / co2 / pm25 / temp / pir
    value: float     # 触发值
    threshold: float # 触发阈值
    message: str     # 最终文案（soft 由 LLM 生成）
    ts: str
```

---

## 4. 规则引擎（rule_engine.py）— 核心

### 4.1 事件分类

| 类别 | 判定 | 级别 | 例子 |
|---|---|---|---|
| 缺失型 | 超时无消息 | 硬告警 | `offline_timeout_s` 秒没收到 → 设备离线 |
| 超限型 | 数值越 warning/critical | 分级 | co2 / pm25 / temp 越阈值 |
| 突变型（后置） | 斜率超阈值 | 软分析 | MVP 先不做 |

### 4.2 分级告警（每传感器一个状态机）

```
normal ──值≥warning──▶ warning ──值≥critical──▶ critical
  ▲                      │                        │
  └────值≤(warning-迟滞)─┘◀──值≤(critical-迟滞)────┘
```

- **warning** → 触发**软分析**（调 LLM 判定故障/波动/传感器异常）
- **critical** → 触发**硬告警**（不调 LLM，直接发）
- **迟滞**：触发阈值 ≠ 恢复阈值，避免边界抖动
- **冷却**：同一 `kind` 在 `cooldown_s` 内不重复发；状态恢复后重发一次"已恢复"

### 4.3 阈值策略（与 ESP32 阈值呼应）

ESP32 已有：CO₂=1200（基线~784）、烟雾=1500（基线~975）。Agent 侧：

```yaml
rules:
  co2:
    warning: 900      # 低于 ESP32，更早预警 → 软分析
    critical: 1200    # = ESP32 阈值，兜底硬告警
    hysteresis: 50
  pm25:
    warning: 1200
    critical: 1500
    hysteresis: 80
  temp:
    warning: 30.0     # 新增（ESP32 没做温度告警）
    critical: 35.0
    hysteresis: 2.0
```

> ⚠️ co2/pm25 是 ADC 估算值，warning 阈值需**实测标定**（在正常环境下跑一阵，取基线以上留裕量）。文档里的 900/1200 是占位。

---

## 5. LLM 软分析（llm_judge.py）

只在 warning 触发时调用。输入 = 事件 + 最近历史窗口 + 多传感器上下文。

**Prompt 模板：**

```
你是智能家居运维助手。一条传感器数据触发了告警，请判断原因并给出结论。

事件：{kind} 当前值 {value}，告警阈值 {threshold}
最近 {window_min} 分钟历史：{history}
关联上下文：温度 {temp}℃、湿度 {humi}%、光照 {lux}、CO₂ {co2}、烟雾 {pm25}、有人 {pir}

只输出 JSON（不要多余文字）：
{"verdict":"fault|fluctuation|sensor_error","confidence":0.85,"summary":"一句话结论","advice":"建议动作"}
```

- `verdict`：fault=故障 / fluctuation=正常波动 / sensor_error=传感器读数异常
- 超时/失败兜底：LLM 不可用时**降级为普通 hard 告警**，不影响告警送达

---

## 6. 通知格式（notifier.py）

### Server酱 v3（第一渠道）

```
POST https://sctapi.ftqq.com/{SENDKEY}.send
title: [SmartHome告警] CO₂ 偏高
desp:
## CO₂ 偏高
- 时间：2026-08-18 21:30
- 当前值：950 ppm（阈值 900）
- 判定：正常波动（置信度 0.85）
- 说明：室内有人且刚关窗，CO₂ 缓慢上升，未达危险
- 建议：开窗通风
```

### 邮件（第二渠道，后置）
`smtplib` 发同样文案，标题带 `[SmartHome]` 前缀。

---

## 7. 配置（config.yaml）

```yaml
mqtt:
  broker: 192.168.4.2
  port: 1883
  topic_sensor: smarthome/sensor
  topic_cmd: smarthome/cmd

rules:
  offline_timeout_s: 30     # 30s 无消息 = 离线
  co2:   { warning: 900,  critical: 1200, hysteresis: 50 }
  pm25:  { warning: 1200, critical: 1500, hysteresis: 80 }
  temp:  { warning: 30.0, critical: 35.0, hysteresis: 2.0 }
  cooldown_s: 300

llm:
  enabled: true
  base_url: https://dashscope.aliyuncs.com/compatible-mode/v1
  model: qwen-plus
  api_key: ${QWEN_API_KEY}      # 从环境变量读，不写死

notify:
  serverchan:
    sendkey: "SCTxxxx"

storage:
  db_path: agent/data.db
  retention_days: 30
```

---

## 8. MVP 任务分解（带验收标准）

| # | 任务 | 验收标准 |
|---|---|---|
| 1 | config.py + logger.py | YAML 能加载、缺字段报错、日志分级输出 |
| 2 | mqtt_client.py | 订阅 `smarthome/sensor`，解析 JSON 成 SensorSample，断线自动重连 |
| 3 | rule_engine.py | 超限型（co2/pm25/temp）+ 缺失型（offline）+ 状态机 + 迟滞 + 冷却跑通 |
| 4 | notifier.py（Server酱） | 硬告警能推到微信 |
| 5 | llm_judge.py | warning 触发 → LLM 判定 → 文案推送；LLM 挂了降级 hard |
| 6 | storage.py | 采样 + 告警落 SQLite，可查最近 N 分钟历史（喂 LLM 用） |
| 7 | main.py | 整体跑起来，稳定运行 |

**联调方式**：用 `mosquitto_pub` 手动往 `smarthome/sensor` 发一条测试 JSON，验证从"收到消息 → 判定 → 微信收到告警"全链路，不依赖真实 ESP32。

---

## 9. MVP 明确不做

- 突变型（斜率）判定
- 邮件渠道（第二渠道后置）
- 每日/周报生成（第二个迭代）
- Web 端回写、自动控制动作
- 云端部署、多租户、鉴权
