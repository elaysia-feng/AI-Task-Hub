import logging
from typing import Optional

from app.database.mysql import Database
from app.model.task import Task

logger = logging.getLogger(__name__)


class TaskRepository:
    def __init__(self, db: Database):
        self._db = db

    def transaction(self):
        """任务快照与事件流水共用同一数据库事务。"""
        return self._db.transaction()

    def insert(self, task: Task) -> Task:
        """插入新任务，回填 MySQL 自增主键。"""
        cursor = self._db.execute(
            """
            INSERT INTO task (
                source, external_task_id, event_type, title, content_preview,
                project_path, open_target, open_url, status,
                created_at, completed_at, viewed_at
            ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
            """,
            (
                task.source,
                task.external_task_id,
                task.event_type,
                task.title,
                task.content_preview,
                task.project_path,
                task.open_target,
                task.open_url,
                task.status,
                task.created_at,
                task.completed_at,
                task.viewed_at,
            ),
        )
        return task.model_copy(update={"id": cursor.lastrowid})

    def update(self, task: Task) -> bool:
        """更新任务字段。

        当前 schema 下 UPDATE 不触碰 unique 列，IntegrityError 理论上不可触发；
        一旦触发说明存在 schema/数据问题，直接抛出而非静默返回 False（M9）。
        若调用方吞掉 False 继续提交事件，会造成任务状态与事件流水不一致（M1）。
        """
        self._db.execute(
            """
            UPDATE task SET
                event_type = %s, title = %s, content_preview = %s,
                project_path = %s, open_target = %s, open_url = %s, status = %s,
                completed_at = %s, viewed_at = %s
            WHERE id = %s
            """,
            (
                task.event_type,
                task.title,
                task.content_preview,
                task.project_path,
                task.open_target,
                task.open_url,
                task.status,
                task.completed_at,
                task.viewed_at,
                task.id,
            ),
        )
        return True

    def lock_many(self, task_ids: list[int]) -> None:
        """批量锁定指定任务行，按 id 升序避免死锁。

        在一个事务内调用本方法后再执行其他操作，可避免逐行加锁导致的
        锁序不一致死锁（T1 锁 A 等 B，T2 锁 B 等 A）。
        """
        if not task_ids:
            return
        placeholders = ",".join("%s" for _ in task_ids)
        self._db.execute(
            f"SELECT id FROM task WHERE id IN ({placeholders}) ORDER BY id FOR UPDATE",
            tuple(task_ids),
        )

    def get_by_id(self, task_id: int, for_update: bool = False) -> Optional[Task]:
        sql = "SELECT * FROM task WHERE id = %s"
        if for_update:
            sql += " FOR UPDATE"
        row = self._db.query_one(sql, (task_id,))
        return Task.model_validate(row) if row else None

    def get_by_external_id(
        self,
        source: str,
        external_task_id: Optional[str],
        for_update: bool = False,
    ) -> Optional[Task]:
        """按 (source, external_task_id) 查找现有任务。

        说明：
        - MySQL 中 `column = NULL` 永远为 FALSE，所以必须使用占位列
          `external_task_id_not_null` 做 NULL 安全查询，否则会绕过幂等去重。
        - `None` 和空字符串 `""` 在本方法中共享同一个去重占位（均折叠为 `""`），
          因此用 `external_task_id=""` 创建的任务与用 `None` 创建的任务会命中同一条
          现有记录。这是设计决策而非 bug，有此需求时请在应用层区分。
        - `for_update=True` 需在事务内使用：唯一索引上对「尚不存在的键」做锁定读
          会取 gap lock，串行化并发到达的相同事件，消除「先查后插」的 TOCTOU（M8）。
        """
        normalized = external_task_id or ""
        sql = "SELECT * FROM task WHERE source = %s AND external_task_id_not_null = %s"
        if for_update:
            sql += " FOR UPDATE"
        row = self._db.query_one(sql, (source, normalized))
        return Task.model_validate(row) if row else None

    def list_by_status(
        self, status: str, limit: int = 200, offset: int = 0
    ) -> tuple[list[Task], bool]:
        """按单个状态分页查询，返回 (行, 是否还有下一页)。

        每个种类（状态）一条独立分页流：桌面端为 6 种状态各维护一套
        offset/hasMore。用 `limit + 1` 探测是否有下一页，避免额外 COUNT；
        `id DESC` 作稳定排序副键，同秒多条任务翻页时也不会错位/重复。
        """
        rows = self._db.query_all(
            "SELECT * FROM task WHERE status = %s "
            "ORDER BY created_at DESC, id DESC LIMIT %s OFFSET %s",
            (status, limit + 1, offset),
        )
        has_more = len(rows) > limit
        return [Task.model_validate(r) for r in rows[:limit]], has_more

    def list_by_statuses(
        self, statuses: tuple[str, ...], limit: int = 200, offset: int = 0
    ) -> tuple[list[Task], bool]:
        """按多个状态合并分页（兼容 view=queue/history 的旧调用方与冒烟脚本）。

        仅用于合并视图；桌面端已改为按单状态分页（见 list_by_status）。
        """
        placeholders = ",".join("%s" for _ in statuses)
        rows = self._db.query_all(
            f"SELECT * FROM task WHERE status IN ({placeholders}) "
            "ORDER BY created_at DESC, id DESC LIMIT %s OFFSET %s",
            (*statuses, limit + 1, offset),
        )
        has_more = len(rows) > limit
        return [Task.model_validate(r) for r in rows[:limit]], has_more

    def count_by_status(self) -> dict[str, int]:
        """各状态任务总数（单条 GROUP BY 查询），供状态 chip/标题显示准确计数。"""
        rows = self._db.query_all("SELECT status, COUNT(*) AS n FROM task GROUP BY status")
        return {r["status"]: r["n"] for r in rows}

    def list_by_statuses_for_update(self, statuses: tuple[str, ...]) -> list[Task]:
        """事务内调用：对符合状态的行加 FOR UPDATE 行锁，消除 TOCTOU。"""
        placeholders = ",".join("%s" for _ in statuses)
        rows = self._db.query_all(
            f"SELECT * FROM task WHERE status IN ({placeholders}) ORDER BY id FOR UPDATE",
            statuses,
        )
        return [Task.model_validate(row) for row in rows]

    def mark_all_viewed(self, task_ids: list[int], viewed_at) -> int:
        """批量将指定任务标记为已读，单条 SQL 完成。"""
        if not task_ids:
            return 0
        placeholders = ",".join("%s" for _ in task_ids)
        cursor = self._db.execute(
            f"UPDATE task SET status = 'VIEWED', viewed_at = %s WHERE id IN ({placeholders})",
            (viewed_at, *task_ids),
        )
        return cursor.rowcount

    def delete(self, task_id: int) -> bool:
        """删除指定任务，事件流水经外键 ON DELETE CASCADE 级联删除。"""
        cursor = self._db.execute("DELETE FROM task WHERE id = %s", (task_id,))
        return cursor.rowcount > 0

    def clear(self) -> int:
        """清空全部任务（事件流水经外键 ON DELETE CASCADE 级联删除），返回删除行数。"""
        cursor = self._db.execute("DELETE FROM task")
        return cursor.rowcount
