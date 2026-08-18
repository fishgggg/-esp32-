"""LLM 软分析判定。

- MockJudge：脱敏模拟，不发网络请求，用简单启发式返回判定（测试版本默认）。
- OpenAICompatJudge：真实 LLM（OpenAI 兼容端点，qwen/doubao 等），real 模式才启用。
"""
import json
import re
from typing import List

from .config import Config
from .models import Event, KIND_LABEL, SensorSample, Verdict


class BaseJudge:
    def judge(self, event: Event, history: List[SensorSample]) -> Verdict:
        raise NotImplementedError


class MockJudge(BaseJudge):
    """脱敏模拟判定：根据越界幅度给出确定性结论。"""

    def judge(self, event: Event, history: List[SensorSample]) -> Verdict:
        label = KIND_LABEL.get(event.kind, event.kind)
        over = event.value - event.threshold
        ratio = over / event.threshold if event.threshold else 0.0

        if ratio <= 0.15:
            return Verdict(
                verdict="fluctuation", confidence=0.85,
                summary=f"{label}轻微越过阈值，属正常波动",
                advice="继续观察即可",
            )
        if ratio <= 0.5:
            return Verdict(
                verdict="fault", confidence=0.7,
                summary=f"{label}持续偏高，疑似设备/环境异常",
                advice="建议现场核查",
            )
        return Verdict(
            verdict="sensor_error", confidence=0.9,
            summary=f"{label}读数异常跳变，疑似传感器故障",
            advice="建议检查传感器接线与校准",
        )


class OpenAICompatJudge(BaseJudge):
    """真实 LLM（OpenAI 兼容端点）。llm_mode=real 才启用。"""

    def __init__(self, cfg: Config, log):
        self.cfg = cfg
        self.log = log

    def judge(self, event: Event, history: List[SensorSample]) -> Verdict:
        import requests

        prompt = build_prompt(event, history)
        resp = requests.post(
            f"{self.cfg.llm_base_url}/chat/completions",
            headers={"Authorization": f"Bearer {self.cfg.llm_api_key}"},
            json={"model": self.cfg.llm_model,
                  "messages": [{"role": "user", "content": prompt}]},
            timeout=15,
        )
        resp.raise_for_status()
        text = resp.json()["choices"][0]["message"]["content"]
        return parse_verdict(text)


def build_prompt(event: Event, history: List[SensorSample]) -> str:
    lines = [
        "你是智能家居运维助手。一条传感器数据触发了告警，请判断原因并给出结论。",
        "",
        f"事件：{event.kind} 当前值 {event.value}，告警阈值 {event.threshold}",
    ]
    if history:
        recent = ", ".join(
            f"co2={s.co2},pm25={s.pm25},temp={s.temp}" for s in history[-10:]
        )
        lines.append(f"最近历史：{recent}")
    lines.append(
        '只输出 JSON：{"verdict":"fault|fluctuation|sensor_error",'
        '"confidence":0.85,"summary":"一句话","advice":"建议"}'
    )
    return "\n".join(lines)


def parse_verdict(text: str) -> Verdict:
    m = re.search(r"\{.*\}", text, re.S)
    if m:
        try:
            d = json.loads(m.group(0))
            return Verdict(
                verdict=d.get("verdict", "fluctuation"),
                confidence=float(d.get("confidence", 0.5)),
                summary=d.get("summary", ""),
                advice=d.get("advice", ""),
            )
        except (ValueError, TypeError):
            pass
    # 解析失败兜底
    return Verdict(verdict="fluctuation", confidence=0.5, summary=text[:100], advice="")


def make_judge(cfg: Config, log) -> BaseJudge:
    if cfg.llm_mode == "real":
        return OpenAICompatJudge(cfg, log)
    return MockJudge()
