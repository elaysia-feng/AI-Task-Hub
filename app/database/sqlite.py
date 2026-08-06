"""SQLite 存储后端，与 MySQL Database 同公开接口。

设计要点：
- 进程内单连接 + threading.RLock，镜像 mysql.py 的并发模型（本地单机并发极低）。
- 连接时 PRAGMA foreign_keys=ON（外键级联生效）、journal_mode=WAL、busy_timeout=5000。
- datetime 经 adapter 落库为 ISO-8601 TEXT，读回时还原为 datetime，API 返回行为与 MySQL 一致。
- MySQL 方言轻量翻译：%s→?、INSERT IGNORE→INSERT OR IGNORE、剥离末尾 FOR UPDATE
  （SQLite 无行锁，写事务用 BEGIN IMMEDIATE 串行化，等效消除 TOCTOU）。
"""

import logging
import os
import re
import sqlite3
import sys
import threading
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

logger = logging.getLogger(__name__)

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


def _resource_path(relative: str) -> Path:
    """PyInstaller onefile 解压目录优先，否则按源码仓库布局解析。"""
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        return Path(meipass) / relative
    return _PROJECT_ROOT / relative


_SCHEMA_PATH = _resource_path("app/database/schema_sqlite.sql")


def _load_dotenv() -> None:
    """复用 mysql.py 的 .env 加载（幂等），保证 AIHUB_SQLITE_PATH 可经 .env 配置。"""
    # 延迟导入避免 sqlite.py 单独导入时触发 pymysql 依赖链
    from app.database.mysql import _load_dotenv as _mysql_load_dotenv

    _mysql_load_dotenv()


# ---- datetime 适配：列存 ISO-8601 TEXT，读回还原 datetime ----
# Python 3.12 起 sqlite3 内置 datetime adapter 会发 DeprecationWarning，显式注册自有实现。
def _adapt_datetime(value: datetime) -> str:
    return value.isoformat(sep=" ")


def _convert_datetime(value: bytes) -> datetime:
    text = value.decode("utf-8")
    try:
        return datetime.fromisoformat(text)
    except ValueError:
        return text  # 非 ISO 文本原样返回，避免污染普通文本列


sqlite3.register_adapter(datetime, _adapt_datetime)
sqlite3.register_converter("DATETIME", _convert_datetime)

# schema_sqlite.sql 中时间列声明为 TEXT，PARSE_DECLTYPES 不会按声明类型触发转换，
# 这里对固定时间列名做显式还原（与 register_converter 同一逻辑），保证返回 datetime。
_DATETIME_COLUMNS = frozenset({"created_at", "completed_at", "viewed_at"})


def _sqlite_path() -> Path:
    """解析 AIHUB_SQLITE_PATH；默认 %APPDATA%\\AI Task Hub\\data.sqlite（打包版与开发版一致）。"""
    env = os.environ.get("AIHUB_SQLITE_PATH")
    if env:
        p = Path(env)
        return p if p.is_absolute() else _PROJECT_ROOT / p
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "AI Task Hub" / "data.sqlite"
    return _PROJECT_ROOT / "data.sqlite"


@dataclass
class SQLiteConfig:
    """SQLite 连接配置：backend 标记 + 数据库文件路径。"""

    backend: str = "sqlite"
    database: str = ""  # 数据库文件路径


class _ExecManyResult:
    """execute_many 的 INSERT OR IGNORE 分支返回值：携带 MySQL cursor.rowcount 语义。

    sqlite3 Cursor.rowcount 为只读且反映「最近一条语句」，而 MySQL INSERT IGNORE
    的 rowcount 是整批实际插入行数。这里在逐行跳过约束违反后累加，用一个小对象
    暴露同样的 .rowcount 契约（唯一消费方 event_repository.insert_many 只读该字段）。
    """

    __slots__ = ("rowcount",)

    def __init__(self, rowcount: int) -> None:
        self.rowcount = rowcount


class SQLiteDatabase:
    """SQLite 访问入口。

    单连接 + RLock 覆盖 uvicorn 事件循环与 FastAPI 线程池的触达；
    check_same_thread=False 允许跨线程在同一锁下复用连接。
    """

    def __init__(self, config: Optional[SQLiteConfig] = None):
        _load_dotenv()
        self.config = config or SQLiteConfig(database=str(_sqlite_path()))
        self._lock = threading.RLock()
        self._path = Path(self.config.database)
        # 默认目录（%APPDATA%\\AI Task Hub）可能尚不存在
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = self._connect()
        self._init_schema()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(
            str(self._path),
            isolation_level=None,  # autocommit 模式，镜像 MySQL autocommit=True 语义
            detect_types=sqlite3.PARSE_DECLTYPES,
            check_same_thread=False,
        )
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA busy_timeout = 5000")
        return conn

    def _init_schema(self) -> None:
        """执行 schema_sqlite.sql 建表（IF NOT EXISTS 幂等）。

        SQLite DDL 参与事务：任一语句失败则整批回滚，无需像 MySQL 那样手动 DROP。
        """
        statements = [
            stmt.strip()
            for stmt in _SCHEMA_PATH.read_text(encoding="utf-8").split(";")
            if stmt.strip()
        ]
        with self.transaction():
            for stmt in statements:
                self._conn.execute(stmt)

    @staticmethod
    def _prepare(sql: str) -> str:
        """MySQL 方言 → SQLite 方言轻量翻译。

        - %s 占位符 → ?
        - INSERT IGNORE → INSERT OR IGNORE
        - 剥离末尾 FOR UPDATE（SQLite 无行锁；写事务靠 BEGIN IMMEDIATE 串行化）
        动态 IN (%s, ...) 同样由 %s→? 覆盖。
        """
        sql = re.sub(r"\s+FOR\s+UPDATE\s*$", "", sql, flags=re.IGNORECASE)
        sql = re.sub(r"\bINSERT\s+IGNORE\b", "INSERT OR IGNORE", sql, flags=re.IGNORECASE)
        return sql.replace("%s", "?")

    @staticmethod
    def _row_to_dict(row: sqlite3.Row) -> dict[str, Any]:
        data = dict(row)
        for col in _DATETIME_COLUMNS:
            if col in data and isinstance(data[col], str):
                try:
                    data[col] = datetime.fromisoformat(data[col])
                except ValueError:
                    pass  # 非 ISO 文本保持原样
        return data

    def execute_many(self, sql: str, params: list[tuple]):
        """批量执行同一条 SQL，返回 cursor（带 rowcount）。

        与 MySQL 对齐：INSERT OR IGNORE 时 rowcount 为实际插入行数（被跳过的行不计）。
        镜像 MySQL INSERT IGNORE 对约束违反（含 FK）按行跳过的语义：SQLite 的
        INSERT OR IGNORE 不抑制 FOREIGN KEY 约束（会抛 IntegrityError），而 MySQL
        的 INSERT IGNORE 会把 FK 错误降级为 warning 并跳过该行——这里对
        INSERT OR IGNORE 语句逐行执行并跳过 IntegrityError 行，避免
        「task 已被删除」这类陈旧引用中断整批写入（event_repository 依赖该行为）。
        """
        prepared = self._prepare(sql)
        with self._lock:
            cursor = self._conn.cursor()
            if "INSERT OR IGNORE" in prepared.upper():
                inserted = 0
                for row in params:
                    try:
                        cursor.execute(prepared, row)
                        inserted += cursor.rowcount
                    except sqlite3.IntegrityError:
                        continue  # 该行违反约束（含 FK），跳过并继续；MySQL INSERT IGNORE 同语义
                return _ExecManyResult(inserted)
            cursor.executemany(prepared, params)
            return cursor

    def execute(self, sql: str, params: tuple = ()):
        """执行写 SQL 并返回 cursor（带 lastrowid/rowcount）。

        注意：必须在 transaction() 块内调用才参与事务；否则在 autocommit 模式下立即提交。
        """
        with self._lock:
            cursor = self._conn.cursor()
            cursor.execute(self._prepare(sql), params)
            return cursor

    def query_one(self, sql: str, params: tuple = ()) -> Optional[dict]:
        """查询单行，返回 dict 或 None。"""
        with self._lock:
            cursor = self._conn.cursor()
            cursor.execute(self._prepare(sql), params)
            row = cursor.fetchone()
            return self._row_to_dict(row) if row is not None else None

    def query_all(self, sql: str, params: tuple = ()) -> list[dict]:
        """查询多行，返回 dict 列表。"""
        with self._lock:
            cursor = self._conn.cursor()
            cursor.execute(self._prepare(sql), params)
            return [self._row_to_dict(row) for row in cursor.fetchall()]

    @contextmanager
    def transaction(self):
        """BEGIN IMMEDIATE 写事务；RLock 允许仓库方法在事务内复用同一连接。

        成功 commit、异常 rollback 并 re-raise。SQLite 无行锁，BEGIN IMMEDIATE
        立即取库级写锁，使「先查后插」的 TOCTOU 串行化（等效 MySQL FOR UPDATE）。
        """
        with self._lock:
            self._conn.execute("BEGIN IMMEDIATE")
            try:
                yield
                self._conn.commit()
            except Exception:
                self._conn.rollback()
                raise

    def close(self) -> None:
        with self._lock:
            self._conn.close()
