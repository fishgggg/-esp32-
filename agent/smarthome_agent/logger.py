"""统一日志。"""
import logging
import sys


def _force_utf8_stdout():
    """Windows 控制台默认 GBK，强制 UTF-8 以正确输出中文/特殊字符（CO₂、emoji 等）。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass


def setup_logger(name: str = "agent", level: int = logging.INFO) -> logging.Logger:
    _force_utf8_stdout()
    logger = logging.getLogger(name)
    if not logger.handlers:
        logger.setLevel(level)
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter(
            "%(asctime)s [%(levelname)s] %(message)s",
            datefmt="%H:%M:%S",
        ))
        logger.addHandler(handler)
        logger.propagate = False
    return logger
