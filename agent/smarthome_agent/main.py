"""真实 MQTT 模式入口：订阅 smarthome/sensor 并运行判定管线。

用法：
    python -m smarthome_agent.main [config.yaml]
依赖 paho-mqtt（见 requirements.txt）。
"""
import sys
import time

from .config import load_config
from .logger import setup_logger
from .llm_judge import make_judge
from .mqtt_client import MQTTListener
from .notifier import make_notifier
from .pipeline import Pipeline
from .rule_engine import RuleEngine
from .storage import Storage


def run(cfg_path: str):
    log = setup_logger()
    cfg = load_config(cfg_path)
    log.info("启动 SmartHome Agent —— 通知=%s，LLM=%s", cfg.notify_mode, cfg.llm_mode)

    storage = Storage(cfg.db_path, cfg.retention_days, log)
    engine = RuleEngine(cfg, log)
    judge = make_judge(cfg, log)
    notifier = make_notifier(cfg, log)
    pipeline = Pipeline(cfg, log, engine, judge, notifier, storage)

    listener = MQTTListener(cfg, pipeline.on_sample, log)
    listener.start()
    log.info("监听 %s ...", cfg.topic_sensor)

    try:
        while True:
            time.sleep(1)
            pipeline.check_offline(time.time())
    except KeyboardInterrupt:
        log.info("收到中断，停止")
    finally:
        listener.stop()
        storage.close()


def main():
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else "config.yaml"
    run(cfg_path)


if __name__ == "__main__":
    main()
