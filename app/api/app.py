import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.api import event_api, task_api, websocket_api
from app.database.mysql import Database
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.service.event_service import EventService
from app.service.task_service import TaskService
from shared.constants import APP_NAME

logger = logging.getLogger(__name__)


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

    @app.get("/api/health")
    async def health() -> dict:
        return {"status": "ok", "service": APP_NAME}

    return app


# uvicorn 以工厂模式装配（uvicorn --factory app.api.app:create_app），
# 避免 import 时即连接数据库的副作用，测试可注入独立配置
