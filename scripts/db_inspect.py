"""数据库诊断：对比 ai_task_hub / ai_task_hub_test 的表与行数，排查数据去向。

仅支持 MySQL：本脚本的 SQL（information_schema、`db`.table 库名前缀）为 MySQL 专属。
用法：AIHUB_DB_BACKEND=mysql .venv/Scripts/python.exe scripts/db_inspect.py
"""

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pymysql

from app.database.mysql import MySQLConfig

_IDENT_RE = re.compile(r"^[A-Za-z0-9_]+$")


def _safe_ident(name: str) -> str:
    """库/表标识符无法参数化，只允许字母数字下划线，防环境变量注入 SQL（安全审查）。"""
    if not _IDENT_RE.fullmatch(name):
        raise SystemExit(f"非法数据库名：{name!r}（仅允许字母/数字/下划线，请检查 AIHUB_MYSQL_DB 配置）")
    return name


def main() -> None:
    backend = os.environ.get("AIHUB_DB_BACKEND", "auto").strip().lower()
    if backend != "mysql":
        raise SystemExit(
            "db_inspect.py 仅支持 MySQL：其 SQL（information_schema、`db`.table 库名前缀）"
            "是 MySQL 专属语法。\n"
            "请设置 AIHUB_DB_BACKEND=mysql 后运行；SQLite 场景请直接查看 "
            "AIHUB_SQLITE_PATH 指定的数据库文件（默认 %APPDATA%\\AI Task Hub\\data.sqlite）。"
        )
    cfg = MySQLConfig.from_env()
    # 库名从配置/环境派生，不再硬编码（LOW：db_inspect.py 硬编码 DB 名）；插值前过白名单
    main_db = _safe_ident(cfg.database)
    test_db = _safe_ident(os.environ.get("AIHUB_MYSQL_TEST_DB", f"{main_db}_test"))
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
        "WHERE table_schema IN (%s, %s) ORDER BY table_schema",
        (main_db, test_db),
    )
    print("=== 表 ===")
    for r in cur.fetchall():
        print(f"  {r['s']}.{r['n']}")

    for db in (main_db, test_db):
        try:
            cur.execute(f"SELECT COUNT(*) c, COALESCE(MAX(id),0) m FROM `{db}`.task")
            t = cur.fetchone()
            cur.execute(f"SELECT COUNT(*) c FROM `{db}`.task_event")
            e = cur.fetchone()
            print(f"{db}: task={t['c']} 行 (max_id={t['m']}), task_event={e['c']} 行")
        except Exception as exc:
            print(f"{db}: {exc}")

    cur.execute(
        f"SELECT id, source, event_type, status, LEFT(COALESCE(title,''), 30) t, created_at "
        f"FROM `{main_db}`.task ORDER BY id DESC LIMIT 10"
    )
    rows = cur.fetchall()
    print(f"=== {main_db}.task 最近行 ===")
    for r in rows:
        print(" ", r["id"], r["source"], r["event_type"], r["status"], r["t"], r["created_at"])
    conn.close()


if __name__ == "__main__":
    main()
