"""模拟演示入口：不联网、不依赖真实 ESP32 / broker / LLM / Server酱。

用脚本化数据走完「正常 -> warning 软分析 -> critical 硬告警 -> 恢复 -> 离线」全链路。

用法：
    python -m smarthome_agent.demo
"""
import time
from datetime import datetime

from .config import load_config
from .logger import setup_logger
from .llm_judge import make_judge
from .notifier import make_notifier
from .pipeline import Pipeline
from .rule_engine import RuleEngine
from .storage import Storage
from .models import SensorSample


def run_demo(cfg_path: str = "config.yaml"):
    log = setup_logger()
    cfg = load_config(cfg_path)
    cfg.offline_timeout_s = 10      # 演示用：10s 无数据即离线，避免真等 30s

    storage = Storage(cfg.db_path, cfg.retention_days, log)
    engine = RuleEngine(cfg, log)
    judge = make_judge(cfg, log)
    notifier = make_notifier(cfg, log)
    pipeline = Pipeline(cfg, log, engine, judge, notifier, storage)

    start = time.time()
    base = dict(temp=25.0, humi=55.0, lux=120, co2=780, pm25=970, pir=False)

    def emit(offset: float, **kw):
        s = dict(base)
        s.update(kw)
        s["ts"] = datetime.fromtimestamp(start + offset).strftime("%Y-%m-%dT%H:%M:%S")
        pipeline.on_sample(SensorSample(**s), start + offset)

    log.info("========== 模拟演示开始 ==========")

    log.info("--- ① 正常数据（无告警） ---")
    for i in range(3):
        emit(i * 2.0)

    log.info("--- ② CO₂ 越过 warning(900) → 软分析 ---")
    emit(6.0, co2=930)
    emit(8.0, co2=960)

    log.info("--- ③ CO₂ 越过 critical(1200) → 硬告警 ---")
    emit(10.0, co2=1250)

    log.info("--- ④ CO₂ 回落 → 恢复通知 ---")
    emit(12.0, co2=850)

    log.info("--- ⑤ 温度越过 warning(30) → 软分析 ---")
    emit(14.0, temp=31.0)

    log.info("--- ⑥ 模拟离线：跳 15s 无数据 → 硬告警 ---")
    emit(16.0)
    pipeline.check_offline(start + 16.0 + 15.0)

    log.info("========== 模拟演示结束 ==========")
    storage.close()


if __name__ == "__main__":
    run_demo()
