import logging
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.api import event_api, integration_api, task_api, websocket_api
from app.database.mysql import Database
from app.logging_config import log_file
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.service.event_service import EventService
from app.service.task_service import TaskService
from shared.constants import APP_NAME, APP_VERSION

logger = logging.getLogger(__name__)

_STARTED_AT = time.time()


def create_app(db: Database | None = None) -> FastAPI:
    database = db or Database()
    task_repo = TaskRepository(database)
    event_repo = EventRepository(database)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        yield
        database.close()

    app = FastAPI(title=f"{APP_NAME} Event Service", lifespan=lifespan)
    app.state.db = database
    app.state.task_service = TaskService(task_repo, event_repo)
    app.state.event_service = EventService(event_repo)

    app.include_router(event_api.router)
    app.include_router(task_api.router)
    app.include_router(websocket_api.router)
    app.include_router(integration_api.router)

    @app.get("/api/health")
    async def health() -> dict:
        """轻量探活：桌面端 2s 轮询依赖，保持零 DB 交互。"""
        return {"status": "ok", "service": APP_NAME}

    @app.get("/api/status")
    async def status() -> dict:
        """深度状态：版本、运行时长、DB 探活与计数、日志位置（设置页/体检用）。"""
        db_ok = True
        task_count = None
        event_count = None
        try:
            database.query_one("SELECT 1 AS one")
            task_count = database.query_one("SELECT COUNT(*) AS c FROM task")["c"]
            event_count = database.query_one("SELECT COUNT(*) AS c FROM task_event")["c"]
        except Exception:
            db_ok = False
            logger.exception("/api/status 数据库探活失败")
        return {
            "status": "ok" if db_ok else "degraded",
            "service": APP_NAME,
            "version": APP_VERSION,
            "uptimeSec": int(time.time() - _STARTED_AT),
            "db": {
                "ok": db_ok,
                "host": database.config.host,
                "port": database.config.port,
                "database": database.config.database,
            },
            "tasks": task_count,
            "events": event_count,
            "logFile": str(log_file()),
        }

    return app


# uvicorn 以工厂模式装配（uvicorn --factory app.api.app:create_app），
# 避免 import 时即连接数据库的副作用，测试可注入独立配置
