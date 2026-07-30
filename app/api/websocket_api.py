import json
import logging

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

logger = logging.getLogger(__name__)

router = APIRouter()


class WebSocketManager:
    """维护 /ws/tasks 上的所有连接，任务变更时向桌面端广播。"""

    def __init__(self):
        self._connections: set[WebSocket] = set()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self._connections.add(websocket)
        logger.info("WebSocket 客户端接入，当前连接数 %d", len(self._connections))

    def disconnect(self, websocket: WebSocket) -> None:
        self._connections.discard(websocket)

    async def broadcast(self, message: dict) -> None:
        """广播任务变更；失效连接自动清理。"""
        if not self._connections:
            return
        payload = json.dumps(message, ensure_ascii=False)
        stale: list[WebSocket] = []
        for ws in self._connections:
            try:
                await ws.send_text(payload)
            except Exception:
                stale.append(ws)
        for ws in stale:
            self._connections.discard(ws)


ws_manager = WebSocketManager()


@router.websocket("/ws/tasks")
async def tasks_ws(websocket: WebSocket):
    await ws_manager.connect(websocket)
    try:
        # 桌面端目前只接收推送；保留读取循环以感知断连
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        ws_manager.disconnect(websocket)
