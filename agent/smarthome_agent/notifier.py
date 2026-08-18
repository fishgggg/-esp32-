"""通知渠道。

- ConsoleNotifier：模拟通知，打印到控制台（测试版本默认）。
- ServerChanNotifier：真实 Server酱 v3（notify_mode=serverchan 且 sendkey 非空才启用）。
"""
from .config import Config


class BaseNotifier:
    def send(self, title: str, content: str) -> None:
        raise NotImplementedError


class ConsoleNotifier(BaseNotifier):
    """脱敏模拟：不联网，只打印到控制台。"""

    def __init__(self, log):
        self.log = log

    def send(self, title: str, content: str) -> None:
        self.log.info("=" * 56)
        self.log.info("📤 通知（console mock）")
        self.log.info("标题: %s", title)
        self.log.info("内容:\n%s", content)
        self.log.info("=" * 56)


class ServerChanNotifier(BaseNotifier):
    """真实 Server酱 v3。"""

    def __init__(self, sendkey: str, log):
        self.sendkey = sendkey
        self.log = log

    def send(self, title: str, content: str) -> None:
        import requests

        url = f"https://sctapi.ftqq.com/{self.sendkey}.send"
        resp = requests.post(url, data={"title": title, "desp": content}, timeout=15)
        resp.raise_for_status()
        self.log.info("Server酱推送成功: %s", resp.json().get("message", "ok"))


def make_notifier(cfg: Config, log) -> BaseNotifier:
    if cfg.notify_mode == "serverchan" and cfg.serverchan_sendkey:
        return ServerChanNotifier(cfg.serverchan_sendkey, log)
    return ConsoleNotifier(log)
