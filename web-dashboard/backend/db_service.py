"""
db_service.py
直接以只读模式并发查询 SQLite history.db 数据库，提供多维时序降采样与质量统计聚合。
"""

import logging
import os
import sqlite3
from typing import Any, Dict, List, Optional

logger = logging.getLogger("weaknet.db")


class DbService:
    def __init__(self, db_path: Optional[str] = None):
        self.db_path = self._resolve_db_path(db_path)
        logger.info("DbService initialized with database: %s", self.db_path)

    def _resolve_db_path(self, custom_path: Optional[str]) -> str:
        if custom_path and os.path.exists(custom_path):
            return custom_path

        env_dir = os.getenv("WEAKNET_DATA_DIR")
        if env_dir:
            p = os.path.join(env_dir, "history.db")
            if os.path.exists(p):
                return p

        candidates = [
            "/home/radxa/weaknet/data/history.db",
            os.path.abspath("./data/history.db"),
            os.path.abspath("../data/history.db"),
            "/tmp/weaknet/data/history.db",
        ]
        for c in candidates:
            if os.path.exists(c):
                return c

        # 默认返回标准路径
        return "/home/radxa/weaknet/data/history.db"

    def _get_connection(self) -> Optional[sqlite3.Connection]:
        if not os.path.exists(self.db_path):
            return None
        try:
            # 使用 URI 模式只读打开，规避并发写锁
            uri = f"file:{os.path.abspath(self.db_path)}?mode=ro"
            conn = sqlite3.connect(uri, uri=True, timeout=5.0)
            conn.row_factory = sqlite3.Row
            return conn
        except Exception as e:
            logger.error("Failed to connect to SQLite (%s): %s", self.db_path, e)
            return None

    def get_db_info(self) -> Dict[str, Any]:
        conn = self._get_connection()
        if not conn:
            return {"exists": False, "path": self.db_path, "records": 0}
        try:
            cur = conn.cursor()
            cur.execute("SELECT COUNT(*) FROM network_history")
            count = cur.fetchone()[0]

            cur.execute("SELECT MIN(ts), MAX(ts) FROM network_history")
            row = cur.fetchone()
            earliest = row[0] if row else ""
            latest = row[1] if row else ""

            size_kb = os.path.getsize(self.db_path) // 1024
            return {
                "exists": True,
                "path": self.db_path,
                "records": count,
                "size_kb": size_kb,
                "earliest": earliest,
                "latest": latest
            }
        except Exception as e:
            return {"exists": True, "path": self.db_path, "error": str(e)}
        finally:
            conn.close()

    def query_history(self, iface: str = "wlan0", last_minutes: int = 60, max_points: int = 100) -> List[Dict[str, Any]]:
        conn = self._get_connection()
        if not conn:
            return []
        try:
            cur = conn.cursor()
            # 兼容 ISO8601 字符串时间戳与 UNIX 时间戳格式
            query = """
                SELECT id, ts, iface, rtt_ms, jitter_ms, rssi_dbm, tcp_loss, quality, overall_score
                FROM network_history
                WHERE iface = ?
                ORDER BY id DESC
                LIMIT ?
            """
            cur.execute(query, (iface, max_points))
            rows = cur.fetchall()

            result = []
            for r in reversed(rows):
                result.append({
                    "id": r["id"],
                    "ts": r["ts"],
                    "iface": r["iface"],
                    "rtt_ms": r["rtt_ms"],
                    "jitter_ms": round(r["jitter_ms"], 2) if r["jitter_ms"] is not None else None,
                    "rssi_dbm": r["rssi_dbm"],
                    "tcp_loss": round(r["tcp_loss"], 4) if r["tcp_loss"] is not None else None,
                    "quality": r["quality"],
                    "score": round(r["overall_score"], 1) if r["overall_score"] is not None else None
                })
            return result
        except Exception as e:
            logger.error("query_history failed: %s", e)
            return []
        finally:
            conn.close()

    def query_quality_distribution(self, hours: int = 24) -> Dict[str, int]:
        conn = self._get_connection()
        if not conn:
            return {"EXCELLENT": 0, "GOOD": 0, "FAIR": 0, "POOR": 0}
        try:
            cur = conn.cursor()
            cur.execute("""
                SELECT quality, COUNT(*) as cnt
                FROM network_history
                GROUP BY quality
            """)
            rows = cur.fetchall()
            dist = {"EXCELLENT": 0, "GOOD": 0, "FAIR": 0, "POOR": 0}
            for r in rows:
                q = str(r["quality"]).upper()
                if q in dist:
                    dist[q] = r["cnt"]
                else:
                    dist[q] = r["cnt"]
            return dist
        except Exception as e:
            logger.error("query_quality_distribution failed: %s", e)
            return {}
        finally:
            conn.close()
