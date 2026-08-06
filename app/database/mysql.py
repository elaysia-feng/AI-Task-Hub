import logging
import os
import sys
import threading
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

import pymysql
from pymysql.cursors import DictCursor

logger = logging.getLogger(__name__)

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


def _resource_path(relative: str) -> Path:
    """PyInstaller onefile 解压目录优先，否则按源码仓库布局解析。"""
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        return Path(meipass) / relative
    return _PROJECT_ROOT / relative


_SCHEMA_PATH = _resource_path("app/database/schema.sql")


def _config_candidates() -> list[Path]:
    """配置文件候选，按优先级排列：显式指定 > exe 同级（打包版）> 用户级 > 仓库开发态。"""
    candidates: list[Path] = []
    if os.environ.get("AIHUB_CONFIG"):
        try:
            p = Path(os.environ["AIHUB_CONFIG"]).resolve(strict=True)
            # 只接受绝对路径，防路径遍历攻击
            if p.is_absolute():
                candidates.append(p)
        except (OSError, ValueError):
            pass  # 静默忽略非法 env 值
    if getattr(sys, "frozen", False):
        # PyInstaller 打包版：_PROJECT_ROOT 指向临时解压目录，无法读仓库 .env。
        # 把 config.env / .env 放在 exe 旁边即可被读取（如 packaging/dist/ 下）。
        exe_dir = Path(sys.executable).resolve().parent
        candidates.append(exe_dir / "config.env")
        candidates.append(exe_dir / ".env")
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates.append(Path(appdata) / "AI Task Hub" / "config.env")
    candidates.append(_PROJECT_ROOT / ".env")
    return candidates


def _load_dotenv() -> None:
    """加载首个存在的候选配置（不覆盖已有环境变量），避免额外依赖 python-dotenv。"""
    for env_path in _config_candidates():
        if not env_path.exists():
            continue
        for line in env_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            value = value.strip()
            # 支持单/双引号包裹的值：密码含 @、# 等字符时用户习惯加引号（如 '@Feng050813'）
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                value = value[1:-1]
            os.environ.setdefault(key.strip(), value)
        return


_load_dotenv()


@dataclass
class MySQLConfig:
    """连接配置：环境变量优先，默认本机 3306 / root / ai_task_hub。"""

    host: str = "127.0.0.1"
    port: int = 3306
    user: str = "root"
    password: str = ""
    database: str = "ai_task_hub"
    backend: str = "mysql"  # 供 /api/status 识别存储后端

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
        conn = pymysql.connect(**kwargs)
        # 5 秒查询超时，防止慢查询长期持锁
        with conn.cursor() as cursor:
            cursor.execute("SET SESSION MAX_EXECUTION_TIME = 5000")
        return conn

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
        import re

        statements = [
            stmt.strip()
            for stmt in _SCHEMA_PATH.read_text(encoding="utf-8").split(";")
            if stmt.strip()
        ]
        created_tables: list[str] = []
        try:
            with self.transaction():
                with self._conn.cursor() as cursor:
                    for stmt in statements:
                        stmt_upper = stmt.upper()
                        table_name: Optional[str] = None
                        if stmt_upper.startswith("CREATE TABLE"):
                            m = re.search(
                                r"CREATE\s+TABLE\s+(?:`([^`]+)`|(\w+))",
                                stmt_upper,
                                re.IGNORECASE,
                            )
                            table_name = (m.group(1) or m.group(2)) if m else None
                        cursor.execute(stmt)
                        if table_name:
                            created_tables.append(table_name)
        except Exception:
            # DDL statements commit automatically in MySQL, so we cannot truly rollback.
            # On failure, drop any tables that were successfully created to leave
            # the schema in a clean state, then re-raise.
            with self._lock:
                try:
                    conn = self._connect(with_database=True)
                    try:
                        with conn.cursor() as cursor:
                            for tbl in reversed(created_tables):
                                cursor.execute(f"DROP TABLE IF EXISTS `{tbl}`")
                    finally:
                        conn.close()
                    self._conn = self._connect(with_database=True)
                except Exception:
                    logger.exception(
                        "schema cleanup after init failure also failed; schema may be partially initialised"
                    )
            raise

    def _cursor(self):
        # 长时间空闲后 MySQL 会断开连接：ping 失败则整体重建连接
        try:
            self._conn.ping(reconnect=False)
        except pymysql.MySQLError:
            self._conn = self._connect(with_database=True)
        return self._conn.cursor()

    def execute_many(self, sql: str, params: list[tuple]):
        """批量执行同一条 SQL，返回 cursor（带 rowcount）。

        注意：本方法默认 autocommit=True 行为，必须在 transaction() 块内调用才能参与同一事务。
        INSERT IGNORE 时 rowcount 为实际插入行数（被跳过的行不计）。
        """
        with self._lock, self._cursor() as cursor:
            cursor.executemany(sql, params)
            return cursor

    def execute(self, sql: str, params: tuple = ()):
        """执行写SQL并返回cursor。注意：必须在transaction()块内调用才参与事务；否则每条语句立即提交。"""
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return cursor

    def query_one(self, sql: str, params: tuple = ()) -> Optional[dict]:
        """查询单行。注意：必须在transaction()块内调用才参与事务；否则每条语句立即提交。"""
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return cursor.fetchone()

    def query_all(self, sql: str, params: tuple = ()) -> list[dict]:
        """查询多行。注意：必须在transaction()块内调用才参与事务；否则每条语句立即提交。"""
        with self._lock, self._cursor() as cursor:
            cursor.execute(sql, params)
            return list(cursor.fetchall())

    @contextmanager
    def transaction(self):
        """在单连接上执行短事务；RLock 允许仓库方法在事务内复用。

        事务期间 autocommit 设为 False，锁保持到事务结束，防止其他线程
        在事务执行期间交错查询同一连接。

        重要约束：必须在 transaction() 块内调用本类其他 DB 方法（execute,
        query_one, query_all, execute_many）才能参与同一事务；每个方法使用
        独立 cursor 但共享 connection 级 autocommit 状态——在事务外调用时每条
        语句自动提交（autocommit=True 的默认行为）。
        """
        with self._lock:
            self._conn.autocommit(False)
            self._conn.begin()
            try:
                yield
                self._conn.commit()
            except Exception:
                # 失败路径在这里统一 rollback；finally 不再重复 rollback（原实现双重回滚，M7）
                self._conn.rollback()
                raise
            finally:
                self._conn.autocommit(True)

    def close(self) -> None:
        with self._lock:
            self._conn.close()
