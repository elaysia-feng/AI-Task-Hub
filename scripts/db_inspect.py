"""数据库诊断：对比 ai_task_hub / test_mysql 的表与行数，排查数据去向。

用法：.venv/Scripts/python.exe scripts/db_inspect.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pymysql

from app.database.mysql import MySQLConfig


def main() -> None:
    cfg = MySQLConfig.from_env()
    conn = pymysql.connect(
        host=cfg.host,
        port=cfg.port,
        user=cfg.user,
        password=cfg.password,
        cursorclass=pymysql.cursors.DictCursor,
    )
    cur = conn.cursor()
    cur.execute(
        "SELECT table_schema AS s, table_name AS n FROM information_schema.tables "
        "WHERE table_schema IN ('ai_task_hub', 'test_mysql') ORDER BY table_schema"
    )
    print("=== 表 ===")
    for r in cur.fetchall():
        print(f"  {r['s']}.{r['n']}")

    for db in ("ai_task_hub", "test_mysql"):
        try:
            cur.execute(f"SELECT COUNT(*) c, COALESCE(MAX(id),0) m FROM `{db}`.task")
            t = cur.fetchone()
            cur.execute(f"SELECT COUNT(*) c FROM `{db}`.task_event")
            e = cur.fetchone()
            print(f"{db}: task={t['c']} 行 (max_id={t['m']}), task_event={e['c']} 行")
        except Exception as exc:
            print(f"{db}: {exc}")

    cur.execute(
        "SELECT id, source, event_type, status, LEFT(COALESCE(title,''), 30) t, created_at "
        "FROM ai_task_hub.task ORDER BY id DESC LIMIT 10"
    )
    rows = cur.fetchall()
    print("=== ai_task_hub.task 最近行 ===")
    for r in rows:
        print(" ", r["id"], r["source"], r["event_type"], r["status"], r["t"], r["created_at"])
    conn.close()


if __name__ == "__main__":
    main()
