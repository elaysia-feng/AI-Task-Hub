"""pytest 全局配置：所有测试打到独立测试库 test_mysql，与真实数据隔离。

必须在任何 app 模块导入之前设置 AIHUB_MYSQL_DB。
MySQL 不可达时全部跳过，不影响无数据库环境下的静态检查。
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

os.environ["AIHUB_MYSQL_DB"] = os.environ.get("AIHUB_MYSQL_TEST_DB", "test_mysql")

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
        pytest.skip(f"MySQL 不可用，跳过测试: {exc}")
    # 测试库重建，保证隔离
    database.execute("DROP TABLE IF EXISTS task_event")
    database.execute("DROP TABLE IF EXISTS task")
    database._init_schema()
    yield database
    database.close()


@pytest.fixture(autouse=True)
def clean_tables(db):
    db.execute("DELETE FROM task_event")
    db.execute("DELETE FROM task")


@pytest.fixture()
def task_repo(db):
    return TaskRepository(db)


@pytest.fixture()
def event_repo(db):
    return EventRepository(db)


@pytest.fixture()
def task_service(task_repo, event_repo):
    return TaskService(task_repo, event_repo)


@pytest.fixture()
def client(db):
    from fastapi.testclient import TestClient

    from app.api.app import create_app

    return TestClient(create_app(db))
