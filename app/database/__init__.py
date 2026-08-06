"""存储后端工厂：按 AIHUB_DB_BACKEND 选择 SQLite / MySQL。

- sqlite（默认）：直接使用 SQLite（本机文件，零依赖开箱即用）；
- mysql：严格 MySQL，连不上即抛错；
- auto：优先 MySQL，连接失败自动降级 SQLite。
"""

import logging
import os

from app.database.mysql import Database, _load_dotenv
from app.database.sqlite import SQLiteDatabase

logger = logging.getLogger(__name__)

# 供类型标注与外部直接引用（create_app 的 db 参数、测试注入等）
StorageBackend = Database | SQLiteDatabase


def create_database() -> StorageBackend:
    """按 AIHUB_DB_BACKEND 创建存储后端实例。

    默认 sqlite（零依赖开箱即用）；显式 auto 时 MySQL 不可用自动兜底 SQLite。
    """
    _load_dotenv()
    backend = os.environ.get("AIHUB_DB_BACKEND", "sqlite").strip().lower()
    if backend == "mysql":
        return Database()
    if backend == "sqlite":
        return SQLiteDatabase()
    # auto 或非法值：优先 MySQL，失败降级 SQLite
    try:
        return Database()
    except Exception:
        logger.warning(
            "MySQL 不可用，自动降级 SQLite（如需强制 MySQL 请设置 AIHUB_DB_BACKEND=mysql）",
            exc_info=True,
        )
        return SQLiteDatabase()
