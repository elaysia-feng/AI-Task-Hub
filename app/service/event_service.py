import json
from typing import Any

from app.repository.event_repository import EventRepository


class EventService:
    """任务生命周期事件查询（任务详情时间线、审计排障用）。"""

    def __init__(self, event_repo: EventRepository):
        self._events = event_repo

    def get_task_timeline(self, task_id: int) -> list[dict[str, Any]]:
        """时间线输出为桌面端契约：camelCase 键 + payload 反序列化为对象。"""
        return [self._shape(row) for row in self._events.list_by_task(task_id)]

    @staticmethod
    def _shape(row: dict) -> dict[str, Any]:
        payload = row.get("raw_payload")
        if isinstance(payload, str):
            try:
                payload = json.loads(payload)
            except ValueError:
                payload = {"raw": payload}
        return {
            "id": row["id"],
            "taskId": row["task_id"],
            "eventType": row["event_type"],
            "occurredAt": row["created_at"],
            "payload": payload or {},
        }
