from typing import Literal, Optional

from fastapi import APIRouter, HTTPException, Path, Query, Request
import logging

logger = logging.getLogger(__name__)

from app.model.task import Task
from app.api.websocket_api import ws_manager
from shared.constants import ALL_STATUSES, COMPLETED_UNREAD_STATUSES, HISTORY_STATUSES, QUEUE_STATUSES

# scope → 删除状态集合：completed=仅已完成未读，queue=待处理四种状态，history=已查看/已忽略，all=全部（None）
_CLEAR_SCOPES: dict[str, Optional[tuple[str, ...]]] = {
    "completed": COMPLETED_UNREAD_STATUSES,
    "queue": QUEUE_STATUSES,
    "history": HISTORY_STATUSES,
    "all": None,
}

router = APIRouter(prefix="/api/tasks")


def _dump(task: Task) -> dict:
    return task.model_dump(mode="json", by_alias=True)


@router.get("")
async def list_tasks(
    request: Request,
    status: Optional[str] = Query(None, description="按单个状态分页（每种类一条独立分页流）"),
    view: Literal["queue", "history"] = Query("queue"),
    limit: int = Query(200, ge=1, le=500),
    offset: int = Query(0, ge=0),
) -> dict:
    """任务列表分页查询。

    - status=单状态：每个种类（状态）一条独立分页流，互不影响 offset/hasMore。
    - view=queue/history：兼容旧调用方与冒烟脚本的合并视图。
    返回 hasMore 供前端翻页；limit 上限 500，防止历史无限增长时一次载入全表。
    """
    task_service = request.app.state.task_service
    if status is not None:
        if status not in ALL_STATUSES:
            raise HTTPException(status_code=422, detail=f"未知任务状态: {status}")
        tasks, has_more = task_service.list_by_status(status, limit, offset)
        return {"tasks": [_dump(t) for t in tasks], "hasMore": has_more}
    tasks, has_more = (
        task_service.get_queue(limit, offset)
        if view == "queue"
        else task_service.get_history(limit, offset)
    )
    return {"tasks": [_dump(t) for t in tasks], "hasMore": has_more}


@router.get("/summary")
async def task_summary(request: Request) -> dict:
    """各状态任务总数（GROUP BY 一条查询），供状态 chip/标题显示准确计数。

    必须注册在 /{task_id} 之前：GET /api/tasks/summary 会被 int 类型路径参数吞掉。
    """
    return {"counts": request.app.state.task_service.status_summary()}


@router.get("/snapshot")
async def task_snapshot(
    request: Request,
    limit: int = Query(100, ge=1, le=500),
) -> dict:
    """返回各状态首屏与准确计数，减少桌面端首次刷新时的 HTTP 往返。"""
    task_service = request.app.state.task_service
    buckets = {}
    for status in ALL_STATUSES:
        tasks, has_more = task_service.list_by_status(status, limit, 0)
        buckets[status] = {
            "tasks": [_dump(task) for task in tasks],
            "hasMore": has_more,
        }
    return {"counts": task_service.status_summary(), "buckets": buckets}


@router.get("/{task_id}")
async def get_task(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    """按 ID 获取任务，供历史任务重新打开等单任务操作使用。"""
    task = request.app.state.task_service.get_task(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail="任务不存在")
    return {"task": _dump(task)}


@router.get("/{task_id}/events")
async def list_task_events(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    """任务事件时间线。"""
    task_service = request.app.state.task_service
    if task_service.get_task(task_id) is None:
        raise HTTPException(status_code=404, detail="任务不存在")
    event_service = request.app.state.event_service
    return {"events": event_service.get_task_timeline(task_id)}


@router.get("/{task_id}/ai-reply")
def get_task_ai_reply(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    """任务对应的最终 AI 答复（尽力而为，不因会话记录缺失而报错）。

    用 sync def：读本地会话记录是阻塞 IO，由 FastAPI 放线程池执行，不卡事件循环。
    - CLAUDE_CODE / CODEX：按需读取本地会话记录（无 DB 迁移，存量任务立即可见）。
    - CHATGPT：从任务最近事件里取扩展上报的 replyText。
    - 缺会话记录 / 读取失败 → 200 + content=null + 中文友好 error，展示层出 muted 提示。
    """
    from app.service import ai_reply

    task_service = request.app.state.task_service
    task = task_service.get_task(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail="任务不存在")

    cached = ai_reply._cache_get(task_id)
    if cached is not None:
        return {"taskId": task_id, "source": task.source, **cached}

    recent_events = (
        request.app.state.event_service.get_task_timeline(task_id)
        if task.source == "CHATGPT"
        else None
    )
    result = ai_reply.get_ai_reply(task, recent_events)
    # 只缓存成功内容：把「未找到答复」错误结果缓存 30s，会在任务恰好完成后仍显示陈旧
    # 错误提示（用户重开详情也不刷新）。错误场景重读成本低，不缓存换取及时性。
    if result["content"] is not None:
        ai_reply._cache_put(task_id, result)
    return {"taskId": task_id, "source": task.source, **result}


@router.post("/read-all")
async def mark_all_viewed(request: Request) -> dict:
    """一键已读：队列中全部完成/失败未读任务标记为已读，广播 tasks_read_all。"""
    count = request.app.state.task_service.mark_all_viewed()
    await ws_manager.broadcast({"type": "tasks_read_all", "count": count})
    return {"success": True, "count": count}


@router.post("/{task_id}/view")
async def mark_viewed(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    task = await _mark_and_broadcast(request, task_id, "view")
    if task is None:
        raise HTTPException(status_code=404, detail="任务不存在")
    return {"success": True, "task": _dump(task)}


@router.post("/{task_id}/ignore")
async def mark_ignored(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    task = await _mark_and_broadcast(request, task_id, "ignore")
    if task is None:
        raise HTTPException(status_code=404, detail="任务不存在")
    return {"success": True, "task": _dump(task)}


@router.delete("")
async def clear_tasks(
    request: Request,
    confirm: bool = Query(False, description="必须为 true 才执行清理"),
    scope: str = Query(
        "all",
        description="completed=只清已完成 / queue=只清待处理 / history=只清历史 / all=全部",
    ),
) -> dict:
    """一键清理指定范围的任务，事件流水经外键级联删除并广播结果。

    Args:
        request: 当前 FastAPI 请求及任务服务上下文。
        confirm: 是否明确确认执行删除。
        scope: 删除范围，可选已完成、待处理、历史或全部。

    Returns:
        包含成功标记与实际删除数量的结果。

    Raises:
        HTTPException: 未确认删除或范围不受支持时抛出 400。
    """
    if not confirm:
        raise HTTPException(status_code=400, detail="需要 confirm=true 才能执行清理")
    statuses = _CLEAR_SCOPES.get(scope)
    if scope not in _CLEAR_SCOPES:
        raise HTTPException(status_code=400, detail=f"未知 scope: {scope}")
    deleted = request.app.state.task_service.clear_all(statuses)
    await ws_manager.broadcast({"type": "tasks_cleared", "deleted": deleted})
    return {"success": True, "deleted": deleted}


@router.delete("/{task_id}")
async def delete_task(request: Request, task_id: int = Path(..., gt=0)) -> dict:
    if not request.app.state.task_service.delete_task(task_id):
        raise HTTPException(status_code=404, detail="任务不存在")
    await ws_manager.broadcast({"type": "task_deleted", "taskId": task_id})
    return {"success": True}


async def _mark_and_broadcast(
    request: Request, task_id: int, action: Literal["view", "ignore"]
) -> Optional[Task]:
    task_service = request.app.state.task_service
    task = (
        task_service.mark_viewed(task_id)
        if action == "view"
        else task_service.mark_ignored(task_id)
    )
    if task is not None:
        await ws_manager.broadcast({
            "type": "task_changed",
            "eventType": "TASK_VIEWED" if action == "view" else "TASK_IGNORED",
            "task": _dump(task),
        })
    return task
