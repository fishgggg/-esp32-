"""管线：把规则引擎事件 -> 生成最终文案 -> 通知 + 落库。"""
from .config import Config
from .models import Event, KIND_LABEL, SensorSample, VERDICT_LABEL


class Pipeline:
    def __init__(self, cfg: Config, log, engine, judge, notifier, storage):
        self.cfg = cfg
        self.log = log
        self.engine = engine
        self.judge = judge
        self.notifier = notifier
        self.storage = storage

    def on_sample(self, sample: SensorSample, now: float):
        self.storage.save_sample(sample)
        for ev in self.engine.handle_sample(sample, now):
            self._handle_event(ev)

    def check_offline(self, now: float):
        ev = self.engine.check_offline(now)
        if ev is not None:
            self._handle_event(ev)

    def _handle_event(self, ev: Event):
        history = self.storage.recent_samples(30)
        severity, title, content = self._build(ev, history)
        self.notifier.send(title, content)
        self.storage.save_alert(severity, ev, content)
        self.log.info("事件: kind=%s state=%s value=%s -> %s",
                      ev.kind, ev.state, ev.value, severity)

    def _build(self, ev: Event, history):
        label = KIND_LABEL.get(ev.kind, ev.kind)

        if ev.state == "offline":
            return "hard", "[SmartHome告警] 设备离线", (
                f"## 设备离线\n"
                f"- 已 {int(ev.value)} 秒未收到数据（超时阈值 {int(ev.threshold)}s）\n"
                f"- 请检查设备供电 / 网络"
            )

        if ev.state == "critical":
            return "hard", f"[SmartHome告警] {label} 超标", (
                f"## {label} 严重超标\n"
                f"- 时间：{ev.ts}\n"
                f"- 当前值：{ev.value}（严重阈值 {ev.threshold}）\n"
                f"- 已触发硬告警，请立即处理"
            )

        if ev.state == "warning":
            verdict = self.judge.judge(ev, history)
            return "soft", f"[SmartHome告警] {label} 偏高", (
                f"## {label} 偏高\n"
                f"- 时间：{ev.ts}\n"
                f"- 当前值：{ev.value}（阈值 {ev.threshold}）\n"
                f"- 判定：{VERDICT_LABEL.get(verdict.verdict, verdict.verdict)}"
                f"（置信度 {verdict.confidence}）\n"
                f"- 说明：{verdict.summary}\n"
                f"- 建议：{verdict.advice}"
            )

        # recovered
        return "info", f"[SmartHome恢复] {label} 已恢复正常", (
            f"## {label} 已恢复\n"
            f"- 时间：{ev.ts}\n"
            f"- 当前值：{ev.value}，已回落到正常范围"
        )
