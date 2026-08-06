"""存储后端契约测试：核心 CRUD、幂等约束、分页、事务回滚、datetime 往返。

默认测试后端为 sqlite（见 conftest.py），此档在 AIHUB_DB_BACKEND=mysql 时同样可跑，
用于验证两个后端对仓库层 SQL 的行为一致（SQL 方言差异由 sqlite.py 内部翻译）。
"""

from datetime import datetime

from app.model.agent_event import AgentEvent
from app.model.task import Task


def make_event(**kwargs) -> AgentEvent:
    defaults = {
        "source": "CODEX",
        "eventType": "TASK_COMPLETED",
        "externalTaskId": "backend-001",
        "title": "后端契约任务",
        "projectPath": "D:/projects/demo",
    }
    defaults.update(kwargs)
    return AgentEvent(**defaults)


def _task(**kwargs) -> Task:
    defaults = {
        "id": 0,
        "source": "CODEX",
        "external_task_id": "crud-1",
        "event_type": "TASK_STARTED",
        "title": "CRUD",
        "status": "RUNNING",
        "created_at": datetime.now().replace(microsecond=0),
    }
    defaults.update(kwargs)
    return Task(**defaults)


class TestCoreCRUD:
    def test_insert_and_query_by_id(self, task_service, db):
        task = task_service.handle_event(make_event())
        assert task.id > 0  # 自增主键回填

        row = db.query_one("SELECT * FROM task WHERE id = %s", (task.id,))
        assert row is not None
        assert row["source"] == "CODEX"
        assert row["external_task_id"] == "backend-001"
        assert row["status"] == "COMPLETED_UNREAD"
        assert row["title"] == "后端契约任务"

    def test_repo_insert_update_delete(self, task_repo, db):
        saved = task_repo.insert(_task())
        assert saved.id > 0
        assert db.query_one("SELECT id FROM task WHERE id = %s", (saved.id,)) is not None

        saved.title = "CRUD 已更新"
        saved.status = "COMPLETED_UNREAD"
        assert task_repo.update(saved)
        row = db.query_one("SELECT * FROM task WHERE id = %s", (saved.id,))
        assert row["title"] == "CRUD 已更新"
        assert row["status"] == "COMPLETED_UNREAD"

        assert task_repo.delete(saved.id)
        assert db.query_one("SELECT id FROM task WHERE id = %s", (saved.id,)) is None

    def test_query_by_external_id(self, task_repo, db):
        saved = task_repo.insert(_task(external_task_id="ext-lookup"))
        found = task_repo.get_by_external_id("CODEX", "ext-lookup")
        assert found is not None and found.id == saved.id
        assert task_repo.get_by_external_id("CODEX", "missing") is None

    def test_clear_deletes_all(self, task_repo, db):
        task_repo.insert(_task(external_task_id="c-1"))
        task_repo.insert(_task(external_task_id="c-2", source="CHATGPT"))
        assert task_repo.clear() == 2
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 0


class TestIdempotency:
    def test_service_merges_duplicate_source_external_id(self, task_service, db):
        first = task_service.handle_event(make_event())
        second = task_service.handle_event(
            make_event(eventType="TASK_FAILED", title="后续事件补全")
        )
        # 同源同 external_task_id 合并到同一任务，不产生新行
        assert first.id == second.id
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 1
        assert second.status == "FAILED_UNREAD"
        assert second.title == "后续事件补全"

    def test_insert_or_ignore_skips_duplicate_key(self, db):
        """唯一约束 (source, external_task_id_not_null) 直接拦截重复插入。"""
        sql = """
            INSERT IGNORE INTO task (
                source, external_task_id, event_type, title, content_preview,
                project_path, open_target, open_url, status,
                created_at, completed_at, viewed_at
            ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """
        ts = datetime.now().replace(microsecond=0)
        params = ("CODEX", "raw-id", "TASK_STARTED", "t", None, None, None, None,
                  "RUNNING", ts, None, None)
        with db.transaction():
            first = db.execute(sql, params)
            second = db.execute(sql, params)
        assert first.rowcount == 1
        assert second.rowcount == 0  # 重复键被跳过
        assert db.query_one(
            "SELECT COUNT(*) AS c FROM task WHERE source = 'CODEX' AND external_task_id = 'raw-id'"
        )["c"] == 1

    def test_null_external_task_id_dedups_per_source(self, task_service, db):
        """NULL external_task_id 折叠为空串占位（generated column），同源合并。

        设计决策：与 MySQL 语义一致（见 task_repository.get_by_external_id 注释），
        避免 NULL 绕过幂等去重造成重复任务。
        """
        first = task_service.handle_event(make_event(externalTaskId=None))
        second = task_service.handle_event(make_event(externalTaskId=None))
        assert first.id == second.id
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 1

    def test_null_external_task_id_different_sources_separate(self, task_service, db):
        codex = task_service.handle_event(make_event(externalTaskId=None, source="CODEX"))
        claude = task_service.handle_event(
            make_event(externalTaskId=None, source="CLAUDE_CODE")
        )
        assert codex.id != claude.id
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 2

    def test_event_insert_many(self, task_service, event_repo, db):
        """批量事件插入：返回实际插入行数，时间线完整追加。"""
        t1 = task_service.handle_event(make_event(externalTaskId="mm-1"))
        t2 = task_service.handle_event(make_event(externalTaskId="mm-2"))
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 2  # 每任务初始事件

        count = event_repo.insert_many([t1.id, t2.id], "TASK_VIEWED")
        assert count == 2
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 4

    def test_event_insert_many_skips_missing_task_fk(self, event_repo, db):
        """FK 违反被按行跳过，不抛错、返回 0（镜像 MySQL INSERT IGNORE 语义）。

        回归：sqlite 的 INSERT OR IGNORE 不抑制 FOREIGN KEY 违反（会抛
        sqlite3.IntegrityError），sqlite.py 在 execute_many 中对 INSERT OR IGNORE
        逐行执行并跳过 IntegrityError 行，保证「task 已被删除」的陈旧引用不中断整批写入。
        """
        count = event_repo.insert_many([999999], "TASK_VIEWED")
        assert count == 0
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 0

    def test_event_insert_many_mixed_valid_and_missing(self, task_service, event_repo, db):
        """混合有效 + 不存在 task_id：只插入有效行，返回实际插入数 1。"""
        t = task_service.handle_event(make_event(externalTaskId="mm-mixed"))
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 1  # handle_event 初始事件

        count = event_repo.insert_many([t.id, 999999], "TASK_VIEWED")
        assert count == 1
        # 只插入有效任务的事件，缺失 task_id 被跳过
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 2
        assert db.query_one(
            "SELECT COUNT(*) AS c FROM task_event WHERE task_id = %s", (t.id,)
        )["c"] == 2


class TestPagination:
    def test_list_by_status_pagination(self, task_service):
        ids: list[int] = []
        for i in range(5):
            t = task_service.handle_event(
                make_event(externalTaskId=f"page-{i}", title=f"分页任务 {i}")
            )
            ids.append(t.id)
        for tid in ids:
            task_service.mark_viewed(tid)

        page1, more1 = task_service.list_by_status("VIEWED", limit=2, offset=0)
        assert len(page1) == 2 and more1 is True
        page2, more2 = task_service.list_by_status("VIEWED", limit=2, offset=2)
        assert len(page2) == 2 and more2 is True
        page3, more3 = task_service.list_by_status("VIEWED", limit=2, offset=4)
        assert len(page3) == 1 and more3 is False

        seen = {t.id for t in page1 + page2 + page3}
        assert seen == set(ids)  # 三页正好覆盖全部 5 条，无重复无遗漏
        assert all(t.status == "VIEWED" for t in page1 + page2 + page3)


class TestTransactionRollback:
    def test_rollback_on_exception(self, db, task_repo):
        task = _task(external_task_id="tx-1")
        try:
            with db.transaction():
                saved = task_repo.insert(task)
                db.execute(
                    "INSERT INTO task_event (task_id, event_type, raw_payload, created_at) "
                    "VALUES (%s, %s, %s, %s)",
                    (saved.id, "TASK_STARTED", None, datetime.now()),
                )
                raise RuntimeError("boom")
        except RuntimeError:
            pass
        # 任务与事件都未落库
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 0
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 0

    def test_commit_persists(self, db, task_repo):
        task = _task(external_task_id="tx-2")
        with db.transaction():
            saved = task_repo.insert(task)
            db.execute(
                "INSERT INTO task_event (task_id, event_type, raw_payload, created_at) "
                "VALUES (%s, %s, %s, %s)",
                (saved.id, "TASK_STARTED", None, datetime.now()),
            )
        assert db.query_one("SELECT COUNT(*) AS c FROM task")["c"] == 1
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 1


class TestDatetimeRoundTrip:
    def test_db_query_returns_datetime_not_str(self, db, task_service):
        task_service.handle_event(make_event())
        row = db.query_one("SELECT * FROM task WHERE external_task_id = %s", ("backend-001",))
        assert isinstance(row["created_at"], datetime)
        assert isinstance(row["completed_at"], datetime)  # TASK_COMPLETED 写 completed_at
        assert row["viewed_at"] is None

    def test_service_returns_datetime(self, task_service):
        task = task_service.handle_event(make_event())
        assert isinstance(task.created_at, datetime)
        assert isinstance(task.completed_at, datetime)

    def test_datetime_exact_roundtrip(self, db, task_repo):
        ts = datetime(2026, 8, 1, 12, 30, 45)  # 微秒为 0，两库均精确往返
        saved = task_repo.insert(
            _task(
                external_task_id="dt-1",
                event_type="TASK_COMPLETED",
                status="COMPLETED_UNREAD",
                created_at=ts,
                completed_at=ts,
            )
        )
        row = db.query_one("SELECT * FROM task WHERE id = %s", (saved.id,))
        assert row["created_at"] == ts
        assert row["completed_at"] == ts


class TestForeignKeyCascade:
    def test_delete_task_cascades_events(self, db, task_service):
        task = task_service.handle_event(make_event())
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 1
        assert task_service.delete_task(task.id)
        assert db.query_one("SELECT COUNT(*) AS c FROM task_event")["c"] == 0
