import time
from collections import defaultdict

from fastapi import APIRouter, HTTPException, Request
from pydantic import Field

from app.model.agent_event import AgentEvent
from app.model.task import Task
from app.api.websocket_api import ws_manager

router = APIRouter(prefix="/api")

_MAX_CONTENT_PREVIEW_LEN = 10_000
_MAX_TITLE_LEN = 512
_MAX_OPEN_URL_LEN = 2048
_MAX_PROJECT_PATH_LEN = 1024
_MAX_EXTERNAL_ID_LEN = 128


class SimpleRateLimiter:
    """简单的 in-memory token bucket rate limiter（per-IP）。

    设计要点：
    - 每分钟 `max_per_minute` 次；超过返回 False
    - 每次 `check` 都清理该 key 桶中超过 60s 的旧时间戳
    - 每 1000 次调用做一次全表 GC，删除空桶（防长期运行内存单调增长）
    """

    def __init__(self, max_per_minute: int = 60):
        self.max = max_per_minute
        self.buckets: dict[str, list[float]] = defaultdict(list)
        self._calls_since_gc: int = 0
        self._gc_interval: int = 1000

    def check(self, key: str) -> bool:
        now = time.time()
        bucket = [t for t in self.buckets[key] if now - t < 60]
        self.buckets[key] = bucket
        if len(bucket) >= self.max:
            self._maybe_gc(now)
            return False
        bucket.append(now)
        self._maybe_gc(now)
        return True

    def _maybe_gc(self, now: float) -> None:
        self._calls_since_gc += 1
        if self._calls_since_gc < self._gc_interval:
            return
        self._calls_since_gc = 0
        # 删除完全过期的桶
        self.buckets = defaultdict(
            list,
            {
                k: v
                for k, v in self.buckets.items()
                if any(now - t < 60 for t in v)
            },
        )


rate_limiter = SimpleRateLimiter(max_per_minute=60)


def _truncate_event(event: AgentEvent) -> AgentEvent:
    updates = {}
    if event.content_preview and len(event.content_preview) > _MAX_CONTENT_PREVIEW_LEN:
        updates["content_preview"] = event.content_preview[:_MAX_CONTENT_PREVIEW_LEN] + "…(truncated)"
    if event.title and len(event.title) > _MAX_TITLE_LEN:
        updates["title"] = event.title[:_MAX_TITLE_LEN] + "…(truncated)"
    if event.open_url and len(event.open_url) > _MAX_OPEN_URL_LEN:
        updates["open_url"] = event.open_url[:_MAX_OPEN_URL_LEN] + "…(truncated)"
    if event.project_path and len(event.project_path) > _MAX_PROJECT_PATH_LEN:
        updates["project_path"] = event.project_path[:_MAX_PROJECT_PATH_LEN] + "…(truncated)"
    if event.external_task_id and len(event.external_task_id) > _MAX_EXTERNAL_ID_LEN:
        updates["external_task_id"] = event.external_task_id[:_MAX_EXTERNAL_ID_LEN] + "…(truncated)"
    if updates:
        return event.model_copy(update=updates)
    return event


@router.post("/events", status_code=201)
async def receive_event(event: AgentEvent, request: Request) -> dict:
    """接收各平台 Adapter 的统一事件，更新任务并广播给桌面端。"""
    # 反代场景优先用 X-Forwarded-For 第一项，避免所有客户端共享同一限速桶
    # request.client 在测试客户端 / Unix socket 场景可能为 None，需兜底（M5）
    client = request.client
    ip = (
        (request.headers.get("x-forwarded-for") or "").split(",")[0].strip()
        or (client.host if client else None)
        or "unknown"
    )
    if not rate_limiter.check(ip):
        raise HTTPException(429, "rate limit exceeded")
    event = _truncate_event(event)
    task_service = request.app.state.task_service
    task: Task = task_service.handle_event(event)

    await ws_manager.broadcast({
        "type": "task_changed",
        "eventType": event.event_type,
        "task": task.model_dump(mode="json", by_alias=True),
    })
    return {"success": True, "taskId": task.id}
