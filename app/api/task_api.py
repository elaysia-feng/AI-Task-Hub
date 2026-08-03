from typing import Literal, Optional

from fastapi import APIRouter, HTTPException, Path, Query, Request
import logging

logger = logging.getLogger(__name__)

from app.model.task import Task
from app.api.websocket_api import ws_manager

router = APIRouter(prefix="/api/tasks")


def _dump(task: Task) -> dict:
    return task.model_dump(mode="json", by_alias=True)


@router.get("")
async def list_tasks(
    request: Request,
    view: Literal["queue", "history"] = Query("queue"),
) -> dict:
    """任务列表：view=queue 队列（进行中/待输入/完成未读/失败未读）；view=history 历史（已读/忽略）。"""
    task_service = request.app.state.task_service
    tasks = task_service.get_queue() if view == "queue" else task_service.get_history()
    return {"tasks": [_dump(t) for t in tasks]}


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
) -> dict:
    """一键清理：删除全部任务（事件流水级联删除），广播 tasks_cleared。"""
    if not confirm:
        raise HTTPException(status_code=400, detail="需要 confirm=true 才能执行清理")
    deleted = request.app.state.task_service.clear_all()
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
