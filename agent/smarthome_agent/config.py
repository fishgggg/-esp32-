"""配置加载：读 config.yaml，展开环境变量，构造 Config。"""
import os
import re
from dataclasses import dataclass, field
from typing import Dict

try:
    import yaml
except ImportError:
    raise SystemExit("缺少 PyYAML，请先运行: pip install pyyaml")

_ENV_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _expand_env(value):
    """把字符串里的 ${NAME} 替换为环境变量值。"""
    if isinstance(value, str):
        return _ENV_RE.sub(lambda m: os.environ.get(m.group(1), ""), value)
    return value


@dataclass
class SensorRule:
    warning: float
    critical: float
    hysteresis: float = 0.0


@dataclass
class Config:
    mqtt_broker: str = "192.168.4.2"
    mqtt_port: int = 1883
    topic_sensor: str = "smarthome/sensor"
    topic_cmd: str = "smarthome/cmd"

    offline_timeout_s: int = 30
    cooldown_s: int = 300
    sensors: Dict[str, SensorRule] = field(default_factory=dict)

    llm_mode: str = "mock"          # mock | real
    llm_base_url: str = ""
    llm_model: str = ""
    llm_api_key: str = ""

    notify_mode: str = "console"    # console | serverchan
    serverchan_sendkey: str = ""

    db_path: str = "data.db"
    retention_days: int = 30


def load_config(path: str) -> Config:
    with open(path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f) or {}

    cfg = Config()

    mqtt = raw.get("mqtt") or {}
    cfg.mqtt_broker = mqtt.get("broker", cfg.mqtt_broker)
    cfg.mqtt_port = int(mqtt.get("port", cfg.mqtt_port))
    cfg.topic_sensor = mqtt.get("topic_sensor", cfg.topic_sensor)
    cfg.topic_cmd = mqtt.get("topic_cmd", cfg.topic_cmd)

    rules = raw.get("rules") or {}
    cfg.offline_timeout_s = int(rules.get("offline_timeout_s", cfg.offline_timeout_s))
    cfg.cooldown_s = int(rules.get("cooldown_s", cfg.cooldown_s))
    for kind, r in (rules.get("sensors") or {}).items():
        cfg.sensors[kind] = SensorRule(
            warning=float(r["warning"]),
            critical=float(r["critical"]),
            hysteresis=float(r.get("hysteresis", 0.0)),
        )

    llm = raw.get("llm") or {}
    cfg.llm_mode = llm.get("mode", "mock")
    cfg.llm_base_url = llm.get("base_url", "")
    cfg.llm_model = llm.get("model", "")
    cfg.llm_api_key = _expand_env(llm.get("api_key", ""))

    notify = raw.get("notify") or {}
    cfg.notify_mode = notify.get("mode", "console")
    sc = notify.get("serverchan") or {}
    cfg.serverchan_sendkey = sc.get("sendkey", "")

    storage = raw.get("storage") or {}
    cfg.db_path = storage.get("db_path", cfg.db_path)
    cfg.retention_days = int(storage.get("retention_days", cfg.retention_days))

    return cfg
