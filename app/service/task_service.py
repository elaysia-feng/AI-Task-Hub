from typing import Optional

from app.model.agent_event import AgentEvent, EventType
from app.model.task import Task, TaskStatus
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.utils import now, to_local_naive
from shared.constants import HISTORY_STATUSES, QUEUE_STATUSES, UNREAD_STATUSES

# 事件类型 → 任务状态 的映射（状态机核心）
_EVENT_TO_STATUS = {
    EventType.TASK_STARTED.value: TaskStatus.RUNNING.value,
    EventType.TASK_NEEDS_INPUT.value: TaskStatus.NEEDS_INPUT.value,
    EventType.TASK_COMPLETED.value: TaskStatus.COMPLETED_UNREAD.value,
    EventType.TASK_FAILED.value: TaskStatus.FAILED_UNREAD.value,
    EventType.TASK_VIEWED.value: TaskStatus.VIEWED.value,
}

_COMPLETED_EVENTS = (EventType.TASK_COMPLETED.value, EventType.TASK_FAILED.value)


class TaskService:
    """统一任务模型服务：所有平台差异在 Adapter 层抹平后，这里只处理状态机。"""

    def __init__(self, task_repo: TaskRepository, event_repo: EventRepository):
        self._tasks = task_repo
        self._events = event_repo

    def handle_event(self, event: AgentEvent) -> Task:
        """接收统一事件，幂等创建/更新任务并记录生命周期。"""
        event_time = to_local_naive(event.created_at) if event.created_at else now()

        task: Optional[Task] = None
        if event.external_task_id:
            task = self._tasks.get_by_external_id(event.source, event.external_task_id)

        if task is None:
            task = Task(
                id=0,
                source=event.source,
                external_task_id=event.external_task_id,
                event_type=event.event_type,
                title=event.title,
                content_preview=event.content_preview,
                project_path=event.project_path,
                open_target=event.open_target,
                open_url=event.open_url,
                status=_EVENT_TO_STATUS[event.event_type],
                created_at=event_time,
                completed_at=event_time if event.event_type in _COMPLETED_EVENTS else None,
                viewed_at=event_time if event.event_type == EventType.TASK_VIEWED.value else None,
            )
            task = self._tasks.insert(task)
        else:
            self._apply_event(task, event, event_time)
            self._tasks.update(task)

        self._events.insert(
            task_id=task.id,
            event_type=event.event_type,
            raw_payload=event.model_dump_json(by_alias=True),
            created_at=event_time,
        )
        return task

    def _apply_event(self, task: Task, event: AgentEvent, event_time) -> None:
        """同一会话的后续事件：更新状态并补充缺失字段。"""
        task.event_type = event.event_type
        task.status = _EVENT_TO_STATUS[event.event_type]
        # 新事件带来更完整的上下文时覆盖旧值
        for field in ("title", "content_preview", "project_path", "open_target", "open_url"):
            value = getattr(event, field)
            if value:
                setattr(task, field, value)

        if event.event_type in _COMPLETED_EVENTS:
            task.completed_at = event_time
            task.viewed_at = None  # 新一轮完成 → 重新进入未读队列
        elif event.event_type == EventType.TASK_VIEWED.value:
            task.viewed_at = event_time

    def get_queue(self) -> list[Task]:
        return self._tasks.list_by_statuses(QUEUE_STATUSES)

    def get_history(self) -> list[Task]:
        return self._tasks.list_by_statuses(HISTORY_STATUSES)

    def get_task(self, task_id: int) -> Optional[Task]:
        return self._tasks.get_by_id(task_id)

    def mark_viewed(self, task_id: int) -> Optional[Task]:
        return self._set_terminal_status(task_id, TaskStatus.VIEWED.value, mark_viewed_at=True)

    def mark_ignored(self, task_id: int) -> Optional[Task]:
        return self._set_terminal_status(task_id, TaskStatus.IGNORED.value)

    def mark_all_viewed(self) -> int:
        """一键已读：队列中所有完成/失败未读任务批量标记为已读，返回处理数量。"""
        count = 0
        for task in self._tasks.list_by_statuses(UNREAD_STATUSES):
            if self.mark_viewed(task.id) is not None:
                count += 1
        return count

    def _set_terminal_status(
        self, task_id: int, status: str, mark_viewed_at: bool = False
    ) -> Optional[Task]:
        task = self._tasks.get_by_id(task_id)
        if task is None:
            return None
        event_time = now()
        task.status = status
        if mark_viewed_at:
            task.viewed_at = event_time
            self._events.insert(task_id, EventType.TASK_VIEWED.value, created_at=event_time)
        self._tasks.update(task)
        return task

    def delete_task(self, task_id: int) -> bool:
        return self._tasks.delete(task_id)

    def clear_all(self) -> int:
        """一键清理：删除全部任务，返回删除数量。"""
        return self._tasks.clear()
