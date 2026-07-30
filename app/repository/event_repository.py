from datetime import datetime
from typing import Optional

from app.database.mysql import Database
from app.utils import now


class EventRepository:
    """任务生命周期事件流水，用于审计、排障与后续离线补偿。"""

    def __init__(self, db: Database):
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

    def list_by_task(self, task_id: int) -> list[dict]:
        return self._db.query_all(
            "SELECT * FROM task_event WHERE task_id = %s ORDER BY created_at ASC",
            (task_id,),
        )
