"""pytest 全局配置：所有测试打到独立测试库 ai_task_hub_test，与真实数据隔离。

必须在任何 app 模块导入之前设置 AIHUB_MYSQL_DB。
MySQL 不可达时全部跳过，不影响无数据库环境下的静态检查。
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

TEST_DB_NAME = os.environ.get("AIHUB_MYSQL_TEST_DB", "ai_task_hub_test")
os.environ["AIHUB_MYSQL_DB"] = TEST_DB_NAME

import pymysql
import pytest

from app.database.mysql import Database, MySQLConfig
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.service.event_service import EventService
from app.service.task_service import TaskService


@pytest.fixture(scope="session")
def db():
    try:
        database = Database(MySQLConfig.from_env())
    except pymysql.err.OperationalError as exc:
        configured = bool(os.environ.get("AIHUB_MYSQL_PASSWORD") or os.environ.get("CI"))
        if configured:
            raise
        pytest.skip(f"MySQL 未配置或不可用，跳过数据库测试: {exc}")
    # 测试库重建，保证隔离
    database.execute("DROP TABLE IF EXISTS task_event")
    database.execute("DROP TABLE IF EXISTS task")
    database._init_schema()
    yield database
    database.close()


@pytest.fixture()
def clean_tables(db):
    # TRUNCATE resets AUTO_INCREMENT, keeping test IDs stable across the session
    # FK_CHECKS=0 是必需的：task_event.task_id → task.id 的外键约束会阻止 TRUNCATE 父表
    db.execute("SET FOREIGN_KEY_CHECKS = 0")
    try:
        db.execute("TRUNCATE TABLE task_event")
        db.execute("TRUNCATE TABLE task")
    finally:
        db.execute("SET FOREIGN_KEY_CHECKS = 1")


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
