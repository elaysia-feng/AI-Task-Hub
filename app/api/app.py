import logging
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware

from app.api import event_api, integration_api, task_api, websocket_api
from app.database.mysql import Database
from app.logging_config import log_file
from app.repository.event_repository import EventRepository
from app.repository.task_repository import TaskRepository
from app.service.event_service import EventService
from app.service.task_service import TaskService
from shared.constants import APP_NAME, APP_VERSION

logger = logging.getLogger(__name__)


class BodySizeLimitMiddleware(BaseHTTPMiddleware):
    """拒绝超过指定大小的 request body，防御 OOM DoS。"""

    def __init__(self, app, max_bytes: int = 1_048_576):
        super().__init__(app)
        self.max_bytes = max_bytes

    async def dispatch(self, request: Request, call_next):
        cl = request.headers.get("content-length")
        if cl:
            try:
                if int(cl) > self.max_bytes:
                    return JSONResponse({"detail": "request body too large"}, status_code=413)
            except ValueError:
                pass
        # chunked 传输没有 content-length，只查头会被绕过（M3）。
        # 流式读 body 边读边判，超限立即 413，避免把超大 body 全量读入内存（OOM 防御）。
        total = 0
        chunks: list[bytes] = []
        async for chunk in request.stream():
            total += len(chunk)
            if total > self.max_bytes:
                return JSONResponse({"detail": "request body too large"}, status_code=413)
            chunks.append(chunk)
        # 已消费的流无法再读，回填给下游 endpoint（Starlette 会优先使用 _body 缓存）
        request._body = b"".join(chunks)
        return await call_next(request)


class SecurityHeadersMiddleware(BaseHTTPMiddleware):
    """设置 HTTP 安全响应头（CSP / X-Frame-Options / X-Content-Type-Options / Referrer-Policy）。

    不含 HSTS：本地 HTTP 应用无 HTTPS 场景，无需强制 upgrade-insecure-requests。
    不含 X-XSS-Protection：现代浏览器默认开启 CSP，该头已废弃。
    """

    async def dispatch(self, request: Request, call_next):
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = "default-src 'self'"
        return response


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

    @app.exception_handler(Exception)
    async def unhandled_exception_handler(request: Request, exc: Exception):
        """全局兜底异常处理器：避免 stack trace 泄露到客户端。"""
        logger.exception("unhandled exception in %s %s", request.method, request.url.path)
        return JSONResponse(status_code=500, content={"detail": "internal server error"})

    app.state.db = database
    app.state.task_service = TaskService(task_repo, event_repo)
    app.state.event_service = EventService(event_repo)

    app.include_router(event_api.router)
    app.include_router(task_api.router)
    app.include_router(websocket_api.router)
    app.include_router(integration_api.router)

    # 限制 request body 大小（DoS 防御）
    app.add_middleware(BodySizeLimitMiddleware, max_bytes=1_048_576)

    # HTTP 安全响应头
    app.add_middleware(SecurityHeadersMiddleware)

    # 限制 origin 避免意外跨域泄露；去掉 credentials（桌面端同源不需要）
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],  # 桌面端同源，web 化时替换为具体域名列表
        allow_methods=["*"],
        allow_headers=["*"],
    )

    @app.get("/api/health")
    async def health() -> dict:
        """轻量探活：桌面端 2s 轮询依赖，附带 DB 可用性检查。"""
        try:
            database.query_one("SELECT 1 AS one")
        except Exception:
            return JSONResponse({"status": "degraded", "service": APP_NAME}, status_code=503)
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
