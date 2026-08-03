"""任务状态机与幂等去重的核心测试。"""

from app.model.agent_event import AgentEvent


def make_event(**kwargs) -> AgentEvent:
    defaults = {
        "source": "CODEX",
        "eventType": "TASK_COMPLETED",
        "externalTaskId": "session-001",
        "title": "修复登录接口",
        "projectPath": "D:/projects/demo",
    }
    defaults.update(kwargs)
    return AgentEvent(**defaults)


class TestHandleEvent:
    def test_completed_event_creates_unread_task(self, task_service):
        task = task_service.handle_event(make_event())

        assert task.id > 0  # MySQL 自增主键回填
        assert task.status == "COMPLETED_UNREAD"
        assert task.completed_at is not None
        assert [t.id for t in task_service.get_queue()] == [task.id]

    def test_dedup_by_source_and_external_id(self, task_service):
        started = task_service.handle_event(
            make_event(eventType="TASK_STARTED", title=None)
        )
        assert started.status == "RUNNING"

        completed = task_service.handle_event(make_event())

        assert completed.id == started.id  # 同一会话事件合并到同一任务
        assert completed.status == "COMPLETED_UNREAD"
        assert completed.title == "修复登录接口"  # 后续事件补全标题
        assert len(task_service.get_queue()) == 1

    def test_started_appears_in_queue_as_running(self, task_service):
        task = task_service.handle_event(make_event(eventType="TASK_STARTED", title="写 README"))
        assert task.status == "RUNNING"
        queue = task_service.get_queue()
        assert [t.id for t in queue] == [task.id]
        assert queue[0].status == "RUNNING"

    def test_needs_input_status(self, task_service):
        task_service.handle_event(make_event(eventType="TASK_STARTED"))
        task = task_service.handle_event(
            make_event(eventType="TASK_NEEDS_INPUT", contentPreview="是否允许执行测试？")
        )

        assert task.status == "NEEDS_INPUT"
        assert task.content_preview == "是否允许执行测试？"

    def test_event_without_external_id_dedups_per_source(self, task_service):
        """修复 DB MEDIUM #3 后，NULL external_task_id 也需按 source 幂等去重。

        历史行为：两次 NULL 事件会创建两条任务（幂等失效）。
        新行为：同源 NULL 事件合并到同一任务，状态/字段被覆盖更新。
        """
        first = task_service.handle_event(make_event(externalTaskId=None))
        second = task_service.handle_event(make_event(externalTaskId=None))

        # 同源 + NULL external_task_id 折叠为同一占位 (source, '')，必须合并
        assert first.id == second.id
        assert len(task_service.get_queue()) == 1

    def test_event_without_external_id_different_sources_create_separate(self, task_service):
        """不同 source 的 NULL external_task_id 各自独立，不应跨源合并。"""
        codex_task = task_service.handle_event(
            make_event(externalTaskId=None, source="CODEX")
        )
        claude_task = task_service.handle_event(
            make_event(externalTaskId=None, source="CLAUDE_CODE")
        )

        assert codex_task.id != claude_task.id
        assert len(task_service.get_queue()) == 2


class TestStatusFlow:
    def test_viewed_leaves_queue_into_history(self, task_service):
        task = task_service.handle_event(make_event())

        viewed = task_service.mark_viewed(task.id)

        assert viewed.status == "VIEWED"
        assert viewed.viewed_at is not None
        assert task_service.get_queue() == []
        assert [t.id for t in task_service.get_history()] == [task.id]

    def test_ignored_leaves_queue(self, task_service):
        task = task_service.handle_event(make_event())

        ignored = task_service.mark_ignored(task.id)

        assert ignored.status == "IGNORED"
        assert task_service.get_queue() == []

    def test_new_completion_resurfaces_viewed_task(self, task_service):
        task = task_service.handle_event(make_event())
        task_service.mark_viewed(task.id)

        resurfaced = task_service.handle_event(make_event())

        assert resurfaced.status == "COMPLETED_UNREAD"
        assert resurfaced.viewed_at is None
        assert [t.id for t in task_service.get_queue()] == [task.id]

    def test_mark_missing_task_returns_none(self, task_service):
        assert task_service.mark_viewed(999999) is None
        assert task_service.mark_ignored(999999) is None

    def test_title_fallback_from_content_preview(self, task_service):
        task = task_service.handle_event(
            make_event(title=None, contentPreview="最新对话：助手回复已完成。")
        )
        assert task.title == "最新对话：助手回复已完成。"

    def test_title_fallback_truncates_long_preview(self, task_service):
        task = task_service.handle_event(make_event(title=None, contentPreview="长" * 100))
        assert len(task.title) == 51  # 50 字 + 省略号
        assert task.title.endswith("…")

    def test_title_fallback_on_later_event(self, task_service):
        task_service.handle_event(make_event(title=None, contentPreview=None))
        healed = task_service.handle_event(
            make_event(title=None, contentPreview="等待确认：是否执行 rm")
        )
        assert healed.title == "等待确认：是否执行 rm"
