import asyncio
import json
import logging
from urllib.parse import urlparse

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

logger = logging.getLogger(__name__)

router = APIRouter()

_MAX_CONNECTIONS = 8
_PING_TIMEOUT = 30


class WebSocketManager:
    """维护 /ws/tasks 上的所有连接，任务变更时向桌面端广播。"""

    def __init__(self):
        self._connections: set[WebSocket] = set()

    async def connect(self, websocket: WebSocket) -> None:
        if len(self._connections) >= _MAX_CONNECTIONS:
            await websocket.close(code=1008, reason="connection limit reached")
            logger.warning("WS 连接数达到上限 %d，拒绝接入", _MAX_CONNECTIONS)
            return
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
        for ws in list(self._connections):
            try:
                await ws.send_text(payload)
            except Exception as exc:
                logger.warning("WS 广播投递失败: %s", exc)
                stale.append(ws)
        for ws in stale:
            self._connections.discard(ws)


ws_manager = WebSocketManager()


def _is_allowed_origin(origin: str) -> bool:
    """精确 origin 校验：仅允许桌面端（空/null/file:///app://）与本地环回。

    之前用 `"localhost" in origin` 子串匹配太松，`https://evil-localhost.attacker.com`
    也能通过。改为解析后只接受完全相等的 host。
    """
    if not origin or origin in ("null", "file://", "app://"):
        # 桌面端（Electron）发起 WS 时 origin 通常为空或 'null'
        return True
    try:
        host = urlparse(origin).hostname or ""
    except Exception:
        return False
    return host in ("localhost", "127.0.0.1")


@router.websocket("/ws/tasks")
async def tasks_ws(websocket: WebSocket):
    origin = websocket.headers.get("origin", "")
    if not _is_allowed_origin(origin):
        await websocket.close(code=1008, reason="forbidden origin")
        return
    await ws_manager.connect(websocket)
    try:
        # 桌面端目前只接收推送；用 timeout receive 做 keepalive，30s 无消息则主动 ping
        while True:
            try:
                await asyncio.wait_for(websocket.receive_text(), timeout=_PING_TIMEOUT)
            except asyncio.TimeoutError:
                # 客户端可能已断开：send 本身也可能抛错，需单独兜住（M6）
                try:
                    await websocket.send_text(json.dumps({"type": "ping"}))
                except Exception as exc:
                    logger.info("WS keepalive ping 发送失败，连接结束: %s", exc)
                    break
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        # 未预期的异常类型（如 send 收到 close 后的 RuntimeError）不抛出，干净收尾（M6）
        logger.info("WS 连接异常结束: %s", exc)
    finally:
        ws_manager.disconnect(websocket)
