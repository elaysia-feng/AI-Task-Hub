import logging
from datetime import datetime
from typing import Optional

import pymysql

from app.database import StorageBackend
from app.utils import now

logger = logging.getLogger(__name__)


class EventRepository:
    """任务生命周期事件流水，用于审计、排障与后续离线补偿。"""

    def __init__(self, db: StorageBackend):
        self._db = db

    def insert(
        self,
        task_id: int,
        event_type: str,
        raw_payload: Optional[str] = None,
        created_at: Optional[datetime] = None,
    ) -> int:
        cursor = self._db.execute(
            """
            INSERT INTO task_event (task_id, event_type, raw_payload, created_at)
            VALUES (%s, %s, %s, %s)
            """,
            (task_id, event_type, raw_payload, created_at or now()),
        )
        return cursor.lastrowid

    def insert_many(
        self,
        task_ids: list[int],
        event_type: str,
        created_at: Optional[datetime] = None,
    ) -> int:
        """批量插入多个 task_id 的同一类型事件；FK 错误（task 已被并发删除）按行忽略。

        使用 INSERT IGNORE + executemany 一次往返；任一 task_id 因 FK violation
        被 MySQL 跳过而不抛错，最终 rowcount 即实际插入行数。

        返回实际插入的行数。
        """
        if not task_ids:
            return 0
        ts = created_at or now()
        rows = [(tid, event_type, None, ts) for tid in task_ids]
        cursor = self._db.execute_many(
            """
            INSERT IGNORE INTO task_event (task_id, event_type, raw_payload, created_at)
            VALUES (%s, %s, %s, %s)
            """,
            rows,
        )
        return cursor.rowcount

    def list_by_task(self, task_id: int) -> list[dict]:
        return self._db.query_all(
            "SELECT * FROM task_event WHERE task_id = %s ORDER BY created_at ASC, id ASC",
            (task_id,),
        )
