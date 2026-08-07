from typing import Literal, Optional

from fastapi import APIRouter, HTTPException, Path, Query, Request
import logging

logger = logging.getLogger(__name__)

from app.model.task import Task
from app.api.websocket_api import ws_manager
from shared.constants import ALL_STATUSES, HISTORY_STATUSES, QUEUE_STATUSES

# scope → 删除状态集合：queue=待处理四种状态，history=已查看/已忽略，all=全部（None）
_CLEAR_SCOPES: dict[str, Optional[tuple[str, ...]]] = {
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
    scope: str = Query("all", description="queue=只清待处理 / history=只清历史 / all=全部"),
) -> dict:
    """一键清理：按 tab 独立清空（queue/history），事件流水级联删除，广播 tasks_cleared。"""
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
