from app.repository.event_repository import EventRepository


class EventService:
    """任务生命周期事件查询（任务详情时间线、审计排障用）。"""

    def __init__(self, event_repo: EventRepository):
        self._events = event_repo

    def get_task_timeline(self, task_id: str) -> list[dict]:
        return self._events.list_by_task(task_id)
