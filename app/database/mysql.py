import os
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

import pymysql
from pymysql.cursors import DictCursor

_SCHEMA_PATH = Path(__file__).parent / "schema.sql"
_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


def _load_dotenv() -> None:
    """加载项目根目录 .env（不覆盖已有环境变量），避免额外依赖 python-dotenv。"""
    env_path = _PROJECT_ROOT / ".env"
    if not env_path.exists():
        return
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip())


_load_dotenv()


@dataclass
class MySQLConfig:
    """连接配置：环境变量优先，默认本机 3306 / root / ai_task_hub。"""

    host: str = "127.0.0.1"
    port: int = 3306
    user: str = "root"
    password: str = ""
    database: str = "ai_task_hub"

    @classmethod
    def from_env(cls) -> "MySQLConfig":
        return cls(
            host=os.environ.get("AIHUB_MYSQL_HOST", "127.0.0.1"),
            port=int(os.environ.get("AIHUB_MYSQL_PORT", "3306")),
            user=os.environ.get("AIHUB_MYSQL_USER", "root"),
            password=os.environ.get("AIHUB_MYSQL_PASSWORD", ""),
            database=os.environ.get("AIHUB_MYSQL_DB", "ai_task_hub"),
        )


class Database:
    """MySQL 访问入口。

    本地单机场景并发极低，单连接 + RLock 即可覆盖
    uvicorn 事件循环与 FastAPI 线程池的触达；断线自动重连。
    """

    def __init__(self, config: Optional[MySQLConfig] = None):
        self.config = config or MySQLConfig.from_env()
        self._lock = threading.RLock()
        self._ensure_database()
        self._conn = self._connect(with_database=True)
        self._init_schema()

    def _connect(self, with_database: bool) -> pymysql.connections.Connection:
        kwargs: dict[str, Any] = dict(
            host=self.config.host,
            port=self.config.port,
            user=self.config.user,
            password=self.config.password,
            charset="utf8mb4",
            autocommit=True,
            cursorclass=DictCursor,
        )
        if with_database:
            kwargs["database"] = self.config.database
        return pymysql.connect(**kwargs)

    def _ensure_database(self) -> None:
        """库不存在则自动创建，免去手动建库步骤。"""
        conn = self._connect(with_database=False)
        try:
            with conn.cursor() as cursor:
                cursor.execute(
                    f"CREATE DATABASE IF NOT EXISTS `{self.config.database}` "
                    "DEFAULT CHARSET utf8mb4 COLLATE utf8mb4_unicode_ci"
                )
        finally:
            conn.close()

    def _init_schema(self) -> None:
        statements = [
            stmt.strip()
            for stmt in _SCHEMA_PATH.read_text(encoding="utf-8").split(";")
            if stmt.strip()
        ]
        with self._lock, self._conn.cursor() as cursor:
            for stmt in statements:
                cursor.execute(stmt)

    def _cursor(self):
        # 长时间空闲后 MySQL 会断开连接：ping 失败则整体重建连接
        try:
            self._conn.ping(reconnect=False)
        except pymysql.MySQLError:
            self._conn = self._connect(with_database=True)
        return self._conn.cursor()

    def execute(self, sql: str, params: tuple = ()):
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return cursor

    def query_one(self, sql: str, params: tuple = ()) -> Optional[dict]:
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return cursor.fetchone()

    def query_all(self, sql: str, params: tuple = ()) -> list[dict]:
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return list(cursor.fetchall())

    def close(self) -> None:
        with self._lock:
            self._conn.close()
