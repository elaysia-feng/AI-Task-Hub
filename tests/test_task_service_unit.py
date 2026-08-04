"""不依赖 MySQL 的任务状态回归测试。"""

from contextlib import nullcontext

from app.model.agent_event import AgentEvent, EventType
from app.model.task import TaskStatus
from app.service.task_service import TaskService


class FakeTaskRepository:
    def __init__(self):
        self.tasks = {}
        self._lock_many_called = []
        self._mark_all_viewed_called = []

    def insert(self, task):
        saved = task.model_copy(update={"id": len(self.tasks) + 1})
        self.tasks[saved.id] = saved
        return saved

    def transaction(self):
        return nullcontext()

    def update(self, task):
        self.tasks[task.id] = task
        return task

    def get_by_id(self, task_id, for_update=False):
        return self.tasks.get(task_id)

    def get_by_external_id(self, source, external_task_id, for_update=False):
        return next(
            (
                task
                for task in self.tasks.values()
                if task.source == source and task.external_task_id == external_task_id
            ),
            None,
        )

    def list_by_statuses(self, statuses, limit=200, offset=0):
        rows = [task for task in self.tasks.values() if task.status in statuses]
        return rows, False  # 与真实实现一致：返回 (行, has_more)

    def list_by_statuses_for_update(self, statuses):
        """Returns same as list_by_statuses; fake is single-threaded so no lock needed."""
        return [task for task in self.tasks.values() if task.status in statuses]

    def delete(self, task_id):
        return self.tasks.pop(task_id, None) is not None

    def clear(self, statuses=None):
        """与真实仓库一致：statuses 为 None 全清，否则只删指定状态。"""
        if statuses is None:
            count = len(self.tasks)
            self.tasks.clear()
            return count
        ids = [tid for tid, t in self.tasks.items() if t.status in statuses]
        for tid in ids:
            self.tasks.pop(tid)
        return len(ids)

    def lock_many(self, task_ids):
        """No-op in fake; records call for assertion."""
        self._lock_many_called.append(list(task_ids))

    def mark_all_viewed(self, task_ids, viewed_at):
        self._mark_all_viewed_called.append((list(task_ids), viewed_at))
        for tid in task_ids:
            if tid in self.tasks:
                self.tasks[tid].status = "VIEWED"
                self.tasks[tid].viewed_at = viewed_at
        return len(task_ids)


class FakeEventRepository:
    def __init__(self):
        self.events = []
        self._insert_many_called = []

    def insert(self, task_id, event_type, raw_payload=None, created_at=None):
        self.events.append((task_id, event_type, raw_payload, created_at))
        return len(self.events)

    def insert_many(self, task_ids, event_type, created_at=None):
        """Records call for assertion; returns count matching the real implementation."""
        self._insert_many_called.append((list(task_ids), event_type, created_at))
        return len(task_ids)


def test_ignore_records_timeline_event():
    tasks = FakeTaskRepository()
    events = FakeEventRepository()
    service = TaskService(tasks, events)
    task = service.handle_event(
        AgentEvent(
            source="CODEX",
            eventType="TASK_COMPLETED",
            externalTaskId="unit-session",
            title="检查忽略事件",
        )
    )

    ignored = service.mark_ignored(task.id)

    assert ignored is not None
    assert ignored.status == "IGNORED"
    assert ignored.event_type == "TASK_IGNORED"
    assert [event[1] for event in events.events] == ["TASK_COMPLETED", "TASK_IGNORED"]


def test_mark_all_viewed_uses_for_update_and_insert_many():
    """Verify mark_all_viewed batches unread tasks to VIEWED without redundant locking.

    Rows are locked once by list_by_statuses_for_update (single FOR UPDATE SELECT),
    so mark_all_viewed must not issue a second lock_many round-trip (M4).
    """
    tasks = FakeTaskRepository()
    events = FakeEventRepository()
    service = TaskService(tasks, events)

    # Create two unread tasks
    task1 = service.handle_event(
        AgentEvent(
            source="CODEX",
            eventType="TASK_COMPLETED",
            externalTaskId="ext-1",
            title="Task 1",
        )
    )
    task2 = service.handle_event(
        AgentEvent(
            source="CODEX",
            eventType="TASK_FAILED",
            externalTaskId="ext-2",
            title="Task 2",
        )
    )
    # Both are now COMPLETED_UNREAD / FAILED_UNREAD
    assert task1.status in (TaskStatus.COMPLETED_UNREAD.value,)
    assert task2.status in (TaskStatus.FAILED_UNREAD.value,)

    result = service.mark_all_viewed()

    assert result == 2
    # No redundant lock_many round-trip (list_by_statuses_for_update already locked the rows)
    assert tasks._lock_many_called == []

    # insert_many was called with both task IDs and TASK_VIEWED event type
    assert len(events._insert_many_called) == 1
    inserted_ids, evt_type, _ = events._insert_many_called[0]
    assert set(inserted_ids) == {task1.id, task2.id}
    assert evt_type == EventType.TASK_VIEWED.value

    # Both tasks are now VIEWED
    assert tasks.tasks[task1.id].status == TaskStatus.VIEWED.value
    assert tasks.tasks[task2.id].status == TaskStatus.VIEWED.value
