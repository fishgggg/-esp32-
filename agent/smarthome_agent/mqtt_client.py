"""MQTT 订阅（真实模式）：订阅 smarthome/sensor，解析 JSON 回调 on_sample。

依赖 paho-mqtt，仅真实模式需要（import 延迟到实例化时）。
"""
import json

from .config import Config
from .models import SensorSample


class MQTTListener:
    def __init__(self, cfg: Config, on_sample, log):
        self.cfg = cfg
        self.on_sample = on_sample
        self.log = log

        import paho.mqtt.client as mqtt

        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, userdata, flags, rc):
        self.log.info("MQTT 已连接，订阅 %s", self.cfg.topic_sensor)
        client.subscribe(self.cfg.topic_sensor)

    def _on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
            self.on_sample(SensorSample.from_dict(data))
        except Exception as e:  # noqa: BLE001
            self.log.warning("解析 MQTT 消息失败: %s", e)

    def _on_disconnect(self, client, userdata, rc):
        self.log.warning("MQTT 断开，自动重连中...")
        self.client.reconnect()

    def start(self):
        self.client.connect(self.cfg.mqtt_broker, self.cfg.mqtt_port, 60)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
