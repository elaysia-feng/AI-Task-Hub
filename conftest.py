"""pytest 全局配置：按 AIHUB_DB_BACKEND 建后端，默认 SQLite（CI 无需 MySQL 即可跑通）。

- sqlite（默认）：临时文件库，测试隔离，不触碰真实数据。
- mysql：打到独立测试库 ai_task_hub_test；MySQL 不可达且未显式配置时跳过。
fixture 全部后端无关；仓库层 SQL 方言差异由 app/database/sqlite.py 内部翻译。
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# 默认测试后端 = sqlite；可用 AIHUB_DB_BACKEND=mysql 切回本地 MySQL 跑同一套用例
os.environ.setdefault("AIHUB_DB_BACKEND", "sqlite")
BACKEND = os.environ["AIHUB_DB_BACKEND"].strip().lower()

# MySQL 测试库名（sqlite 模式用不到，保留以便 MySQL 模式与 status 断言复用）
TEST_DB_NAME = os.environ.get("AIHUB_MYSQL_TEST_DB", "ai_task_hub_test")
os.environ["AIHUB_MYSQL_DB"] = TEST_DB_NAME

import pytest

from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.service.task_service import TaskService


@pytest.fixture(scope="session")
def db(tmp_path_factory):
    """按 AIHUB_DB_BACKEND 建后端；非 mysql（含 auto）固定用 sqlite 临时文件。"""
    from app.database import create_database

    if BACKEND == "mysql":
        import pymysql

        try:
            database = create_database()
        except pymysql.err.OperationalError as exc:
            configured = bool(os.environ.get("AIHUB_MYSQL_PASSWORD") or os.environ.get("CI"))
            if configured:
                raise
            pytest.skip(f"MySQL 未配置或不可用，跳过数据库测试: {exc}")
        # 测试库重建，保证隔离（sqlite 分支用临时文件，天然隔离）
        database.execute("DROP TABLE IF EXISTS task_event")
        database.execute("DROP TABLE IF EXISTS task")
        database._init_schema()
    else:
        # sqlite / auto：临时文件库，避免污染真实数据
        sqlite_path = tmp_path_factory.mktemp("db") / "test.sqlite"
        os.environ["AIHUB_SQLITE_PATH"] = str(sqlite_path)
        os.environ["AIHUB_DB_BACKEND"] = "sqlite"  # 测试固定 sqlite，不依赖 MySQL 可用性
        database = create_database()

    yield database
    database.close()


@pytest.fixture()
def clean_tables(db):
    """清空任务与事件表。DELETE FROM 两库通用（FK 级联删除），
    不再用 TRUNCATE（MySQL 下会被外键阻塞）与 SET FOREIGN_KEY_CHECKS。"""
    db.execute("DELETE FROM task_event")
    db.execute("DELETE FROM task")


@pytest.fixture()
def task_repo(db, clean_tables):
    return TaskRepository(db)


@pytest.fixture()
def event_repo(db, clean_tables):
    return EventRepository(db)


@pytest.fixture()
def task_service(task_repo, event_repo):
    return TaskService(task_repo, event_repo)


@pytest.fixture()
def client(db, clean_tables):
    from fastapi.testclient import TestClient

    from app.api.app import create_app

    return TestClient(create_app(db))
