"""数据结构定义。"""
from dataclasses import dataclass
from typing import List, Optional


KIND_LABEL = {
    "co2": "CO₂",
    "pm25": "烟雾",
    "temp": "温度",
    "offline": "设备",
}

VERDICT_LABEL = {
    "fault": "设备故障",
    "fluctuation": "正常波动",
    "sensor_error": "传感器异常",
}


@dataclass
class SensorSample:
    """一条传感器数据（对应 ESP32 上报的 smarthome/sensor JSON）。"""
    ts: str = ""
    temp: float = 0.0
    humi: float = 0.0
    lux: int = 0
    co2: int = 0
    pm25: int = 0
    pir: bool = False

    @classmethod
    def from_dict(cls, d: dict) -> "SensorSample":
        return cls(
            ts=str(d.get("ts", "")),
            temp=float(d.get("temp", 0.0)),
            humi=float(d.get("humi", 0.0)),
            lux=int(d.get("lux", 0)),
            co2=int(d.get("co2", 0)),
            pm25=int(d.get("pm25", 0)),
            pir=bool(d.get("pir", False)),
        )


@dataclass
class Event:
    """规则引擎输出的事件（尚未生成最终文案）。"""
    kind: str            # offline / co2 / pm25 / temp
    state: str           # warning / critical / recovered / offline
    value: float
    threshold: float
    ts: str = ""


@dataclass
class Verdict:
    """LLM 判定结果。"""
    verdict: str = "fluctuation"   # fault / fluctuation / sensor_error
    confidence: float = 0.5
    summary: str = ""
    advice: str = ""
