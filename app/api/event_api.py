from fastapi import APIRouter, Request

from app.model.agent_event import AgentEvent
from app.model.task import Task
from app.api.websocket_api import ws_manager

router = APIRouter(prefix="/api")


@router.post("/events", status_code=201)
async def receive_event(event: AgentEvent, request: Request) -> dict:
    """接收各平台 Adapter 的统一事件，更新任务并广播给桌面端。"""
    task_service = request.app.state.task_service
    task: Task = task_service.handle_event(event)

    await ws_manager.broadcast({
        "type": "task_changed",
        "eventType": event.event_type,
        "task": task.model_dump(mode="json", by_alias=True),
    })
    return {"success": True, "taskId": task.id}
