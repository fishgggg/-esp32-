"""SQLite 存储：采样历史 + 告警记录。

MVP 简化：check_same_thread=False，真实并发场景需加锁。
"""
import os
import sqlite3

from .models import Event, SensorSample


class Storage:
    def __init__(self, db_path: str, retention_days: int, log):
        self.db_path = db_path
        self.retention_days = retention_days
        self.log = log
        os.makedirs(os.path.dirname(db_path) or ".", exist_ok=True)
        self.conn = sqlite3.connect(db_path, check_same_thread=False)
        self._init()

    def _init(self):
        c = self.conn.cursor()
        c.execute("""CREATE TABLE IF NOT EXISTS samples(
            ts TEXT, temp REAL, humi REAL, lux INTEGER,
            co2 INTEGER, pm25 INTEGER, pir INTEGER)""")
        c.execute("""CREATE TABLE IF NOT EXISTS alerts(
            ts TEXT, severity TEXT, kind TEXT, state TEXT,
            value REAL, threshold REAL, message TEXT)""")
        self.conn.commit()

    def save_sample(self, s: SensorSample):
        self.conn.execute(
            "INSERT INTO samples VALUES (?,?,?,?,?,?,?)",
            (s.ts, s.temp, s.humi, s.lux, s.co2, s.pm25, int(s.pir)),
        )
        self.conn.commit()

    def save_alert(self, severity: str, event: Event, message: str):
        self.conn.execute(
            "INSERT INTO alerts VALUES (?,?,?,?,?,?,?)",
            (event.ts, severity, event.kind, event.state,
             event.value, event.threshold, message),
        )
        self.conn.commit()

    def recent_samples(self, n: int = 30):
        """按时间升序返回最近 n 条采样（供 LLM 上下文使用）。"""
        rows = self.conn.execute(
            "SELECT ts,temp,humi,lux,co2,pm25,pir FROM samples "
            "ORDER BY ts DESC LIMIT ?", (n,),
        ).fetchall()
        return [
            SensorSample(ts=r[0], temp=r[1], humi=r[2], lux=r[3],
                         co2=r[4], pm25=r[5], pir=bool(r[6]))
            for r in reversed(rows)
        ]

    def close(self):
        self.conn.close()
