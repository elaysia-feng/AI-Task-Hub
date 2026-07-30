from typing import Optional

from app.database.mysql import Database
from app.model.task import Task


class TaskRepository:
    def __init__(self, db: Database):
        self._db = db

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

    def update(self, task: Task) -> Task:
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
        return task

    def get_by_id(self, task_id: int) -> Optional[Task]:
        row = self._db.query_one("SELECT * FROM task WHERE id = %s", (task_id,))
        return Task.model_validate(row) if row else None

    def get_by_external_id(self, source: str, external_task_id: str) -> Optional[Task]:
        row = self._db.query_one(
            "SELECT * FROM task WHERE source = %s AND external_task_id = %s",
            (source, external_task_id),
        )
        return Task.model_validate(row) if row else None

    def list_by_statuses(self, statuses: tuple[str, ...]) -> list[Task]:
        placeholders = ",".join("%s" for _ in statuses)
        rows = self._db.query_all(
            f"SELECT * FROM task WHERE status IN ({placeholders}) ORDER BY created_at DESC",
            statuses,
        )
        return [Task.model_validate(row) for row in rows]

    def delete(self, task_id: int) -> bool:
        cursor = self._db.execute("DELETE FROM task WHERE id = %s", (task_id,))
        return cursor.rowcount > 0

    def clear(self) -> int:
        """清空全部任务（事件流水经外键 ON DELETE CASCADE 级联删除），返回删除行数。"""
        cursor = self._db.execute("DELETE FROM task")
        return cursor.rowcount
