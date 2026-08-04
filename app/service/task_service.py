import logging
from typing import Optional

from app.model.agent_event import AgentEvent, EventType
from app.model.task import Task, TaskStatus
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.utils import now, to_local_naive
from shared.constants import ALL_STATUSES, HISTORY_STATUSES, QUEUE_STATUSES, UNREAD_STATUSES

logger = logging.getLogger(__name__)

# 事件类型 → 任务状态 的映射（状态机核心）
_EVENT_TO_STATUS = {
    EventType.TASK_STARTED.value: TaskStatus.RUNNING.value,
    EventType.TASK_NEEDS_INPUT.value: TaskStatus.NEEDS_INPUT.value,
    EventType.TASK_COMPLETED.value: TaskStatus.COMPLETED_UNREAD.value,
    EventType.TASK_FAILED.value: TaskStatus.FAILED_UNREAD.value,
    EventType.TASK_VIEWED.value: TaskStatus.VIEWED.value,
    EventType.TASK_IGNORED.value: TaskStatus.IGNORED.value,
}

_COMPLETED_EVENTS = (EventType.TASK_COMPLETED.value, EventType.TASK_FAILED.value)
_TASK_IGNORED_EVENT = EventType.TASK_IGNORED.value

_FALLBACK_TITLE_LEN = 50


def _derive_title(event: AgentEvent) -> Optional[str]:
    """事件未携带标题时从内容摘要派生（如旧版扩展/钩子的无标题事件）。"""
    preview = (event.content_preview or "").strip().replace("\n", " ")
    if not preview:
        return None
    return preview[:_FALLBACK_TITLE_LEN] + ("…" if len(preview) > _FALLBACK_TITLE_LEN else "")


class TaskService:
    """统一任务模型服务：所有平台差异在 Adapter 层抹平后，这里只处理状态机。"""

    def __init__(self, task_repo: TaskRepository, event_repo: EventRepository):
        self._tasks = task_repo
        self._events = event_repo

    def handle_event(self, event: AgentEvent) -> Task:
        """接收统一事件，幂等创建/更新任务并记录生命周期。"""
        event_time = to_local_naive(event.created_at) if event.created_at else now()
        # model_dump_json 是纯内存序列化，放到事务外执行，避免持有事务锁（M2）
        raw_payload = event.model_dump_json(by_alias=True)

        with self._tasks.transaction():
            task: Optional[Task] = None
            # 总是尝试按 (source, external_task_id) 去重；NULL 也会折叠为空串命中幂等约束。
            # 用 FOR UPDATE 锁定读：唯一索引上对不存在的键取 gap lock，并发相同事件串行化（M8）
            task = self._tasks.get_by_external_id(event.source, event.external_task_id, for_update=True)

            if task is None:
                new_status = _EVENT_TO_STATUS.get(event.event_type)
                if new_status is None:
                    logger.warning(
                        "unknown event type %r received for new task, defaulting to RUNNING",
                        event.event_type,
                    )
                    new_status = TaskStatus.RUNNING.value
                task = Task(
                    id=0,
                    source=event.source,
                    external_task_id=event.external_task_id,
                    event_type=event.event_type,
                    title=event.title or _derive_title(event),
                    content_preview=event.content_preview,
                    project_path=event.project_path,
                    open_target=event.open_target,
                    open_url=event.open_url,
                    status=new_status,
                    created_at=event_time,
                    completed_at=event_time if event.event_type in _COMPLETED_EVENTS else None,
                    viewed_at=event_time if event.event_type == EventType.TASK_VIEWED.value else None,
                )
                task = self._tasks.insert(task)
            else:
                self._apply_event(task, event, event_time)
                if not self._tasks.update(task):
                    # 任务更新失败 → 整体回滚，事件不落库，避免「事件已提交但任务状态未更新」的不一致（M1）
                    logger.error("task update failed for id=%s in handle_event, rolling back", task.id)
                    raise RuntimeError(f"task update failed for id={task.id}")

            self._events.insert(
                task_id=task.id,
                event_type=event.event_type,
                raw_payload=raw_payload,
                created_at=event_time,
            )
        return task

    def _apply_event(self, task: Task, event: AgentEvent, event_time) -> None:
        """同一会话的后续事件：更新状态并补充缺失字段。"""
        task.event_type = event.event_type
        new_status = _EVENT_TO_STATUS.get(event.event_type)
        if new_status is None:
            new_status = task.status  # 未知 event_type 保持现状，不崩
        task.status = new_status
        # 新事件带来更完整的上下文时覆盖旧值
        for field in ("title", "content_preview", "project_path", "open_target", "open_url"):
            value = getattr(event, field)
            if value:
                setattr(task, field, value)
        if not task.title:
            task.title = _derive_title(event)

        if event.event_type in _COMPLETED_EVENTS:
            task.completed_at = event_time
            task.viewed_at = None  # 新一轮完成 → 重新进入未读队列
        elif event.event_type == EventType.TASK_VIEWED.value:
            task.viewed_at = event_time
        elif event.event_type in (
            EventType.TASK_STARTED.value,
            EventType.TASK_NEEDS_INPUT.value,
        ):
            task.viewed_at = None  # 重新进入活跃态 → 清除已读标记

    def list_by_status(self, status: str, limit: int = 200, offset: int = 0) -> tuple[list[Task], bool]:
        """按单个状态分页：每个种类一条独立分页流。"""
        return self._tasks.list_by_status(status, limit=limit, offset=offset)

    def status_summary(self) -> dict[str, int]:
        """各状态任务总数（缺失状态补 0），供状态 chip/标题显示准确计数。"""
        counts = self._tasks.count_by_status()
        return {status: counts.get(status, 0) for status in ALL_STATUSES}

    def get_queue(self, limit: int = 200, offset: int = 0) -> tuple[list[Task], bool]:
        return self._tasks.list_by_statuses(QUEUE_STATUSES, limit=limit, offset=offset)

    def get_history(self, limit: int = 200, offset: int = 0) -> tuple[list[Task], bool]:
        return self._tasks.list_by_statuses(HISTORY_STATUSES, limit=limit, offset=offset)

    def get_task(self, task_id: int) -> Optional[Task]:
        return self._tasks.get_by_id(task_id)

    def mark_viewed(self, task_id: int) -> Optional[Task]:
        return self._set_terminal_status(
            task_id,
            TaskStatus.VIEWED.value,
            EventType.TASK_VIEWED.value,
            mark_viewed_at=True,
        )

    def mark_ignored(self, task_id: int) -> Optional[Task]:
        return self._set_terminal_status(
            task_id,
            TaskStatus.IGNORED.value,
            _TASK_IGNORED_EVENT,
        )

    def mark_all_viewed(self) -> int:
        """一键已读：队列中所有完成/失败未读任务批量标记为已读，单次 SQL 完成。"""
        with self._tasks.transaction():
            # list_by_statuses_for_update 已按 id 升序加 FOR UPDATE 行锁（同一事务内），
            # 消除 TOCTOU；无需再 lock_many 重复加锁（M4）
            tasks = self._tasks.list_by_statuses_for_update(UNREAD_STATUSES)
            if not tasks:
                return 0
            event_time = now()
            task_ids = [t.id for t in tasks]
            self._events.insert_many(task_ids, EventType.TASK_VIEWED.value, created_at=event_time)
            return self._tasks.mark_all_viewed(task_ids, event_time)

    def _set_terminal_status(
        self,
        task_id: int,
        status: str,
        event_type: str,
        mark_viewed_at: bool = False,
    ) -> Optional[Task]:
        event_time = now()
        with self._tasks.transaction():
            task = self._tasks.get_by_id(task_id, for_update=True)
            if task is None:
                return None
            task.status = status
            task.event_type = event_type
            if mark_viewed_at:
                task.viewed_at = event_time
            # 先更新任务再写事件：更新失败则整体回滚，不留下「事件已提交但状态未变」的半套数据（M1）
            if not self._tasks.update(task):
                logger.error("task update failed for id=%s in _set_terminal_status, rolling back", task_id)
                raise RuntimeError(f"task update failed for id={task_id}")
            self._events.insert(task_id, event_type, created_at=event_time)
        return task

    def delete_task(self, task_id: int) -> bool:
        return self._tasks.delete(task_id)

    def clear_all(self, statuses: Optional[tuple[str, ...]] = None) -> int:
        """一键清理：statuses 为 None 删全部，否则只删指定状态，返回删除数量。"""
        return self._tasks.clear(statuses)
