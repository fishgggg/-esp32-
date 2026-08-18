"""规则引擎：超限型（阈值状态机）+ 缺失型（离线检测）。

每个传感器一个三级状态机 normal → warning → critical，带迟滞防抖动。
- warning 触发软分析（交给 LLM 判定）
- critical 触发硬告警（不依赖 LLM，直接发）
- 缺失型：超过 offline_timeout_s 没收到任何消息 → 设备离线硬告警
"""
import time
from typing import List, Optional

from .config import Config
from .models import Event, SensorSample


class RuleEngine:
    def __init__(self, cfg: Config, log):
        self.cfg = cfg
        self.log = log
        self._state = {k: "normal" for k in cfg.sensors}
        self._last_sent = {}          # kind -> epoch，软告警冷却用
        self._last_msg_ts: Optional[float] = None
        self._offline_alerted = False

    def handle_sample(self, sample: SensorSample, now: float) -> List[Event]:
        """收到一条数据，评估所有传感器阈值，返回触发的事件列表。"""
        self._last_msg_ts = now
        self._offline_alerted = False
        events: List[Event] = []
        for kind, rule in self.cfg.sensors.items():
            val = getattr(sample, kind, None)
            if val is None:
                continue
            ev = self._step(kind, rule, float(val), now, sample.ts)
            if ev is not None:
                events.append(ev)
        return events

    def check_offline(self, now: float) -> Optional[Event]:
        """周期性调用：超过 offline_timeout_s 没收到消息 → 设备离线。"""
        if self._last_msg_ts is None or self._offline_alerted:
            return None
        gap = now - self._last_msg_ts
        if gap > self.cfg.offline_timeout_s:
            self._offline_alerted = True
            return Event(
                kind="offline", state="offline",
                value=gap, threshold=self.cfg.offline_timeout_s,
                ts=time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(now)),
            )
        return None

    def _step(self, kind: str, rule, value: float, now: float, ts: str) -> Optional[Event]:
        prev = self._state.get(kind, "normal")

        # 原始分级
        if value >= rule.critical:
            level = "critical"
        elif value >= rule.warning:
            level = "warning"
        else:
            level = "normal"

        # 下降方向加迟滞，避免在阈值附近反复横跳
        if level == "normal" and prev == "warning" and value > rule.warning - rule.hysteresis:
            level = "warning"
        if level in ("normal", "warning") and prev == "critical" and value > rule.critical - rule.hysteresis:
            level = "critical"

        if level == prev:
            return None
        self._state[kind] = level

        # 恢复正常
        if level == "normal":
            return Event(kind=kind, state="recovered", value=value,
                         threshold=rule.warning, ts=ts)

        # 升级告警：warning 走冷却，critical 始终发送（安全兜底）
        if level == "warning" and not self._cooldown_ok(kind, now):
            return None
        self._last_sent[kind] = now
        threshold = rule.critical if level == "critical" else rule.warning
        return Event(kind=kind, state=level, value=value, threshold=threshold, ts=ts)

    def _cooldown_ok(self, kind: str, now: float) -> bool:
        last = self._last_sent.get(kind, 0.0)
        return (now - last) >= self.cfg.cooldown_s
