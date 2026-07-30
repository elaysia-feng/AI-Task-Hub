"""端到端冒烟：独立端口后端实例上跑完整事件生命周期。

用法：
    .venv/Scripts/python.exe scripts/e2e_smoke.py

- 拉起 AIHUB_PORT=17899 + AIHUB_MYSQL_DB=test_mysql 的隔离后端（不碰正式库/正式端口）
- 断言：health → status → 事件 → 队列 → 时间线契约 → read-all → clear → WebSocket 广播
- 结束自动清理（清空测试数据、终止子进程）
"""

import asyncio
import json
import os
import subprocess
import time
import urllib.request
from pathlib import Path

import websockets

PORT = 17899
BASE = f"http://127.0.0.1:{PORT}"
REPO_ROOT = Path(__file__).resolve().parent.parent


def http(method: str, path: str, body: dict | None = None) -> dict:
    """绕过系统代理访问本地后端。"""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        f"{BASE}{path}", data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=10) as res:
        return json.loads(res.read().decode())


def wait_health(timeout: float = 30) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if http("GET", "/api/health")["status"] == "ok":
                return
        except Exception:
            time.sleep(0.5)
    raise RuntimeError("后端冒烟实例启动超时")


def main() -> None:
    env = os.environ.copy()
    env["AIHUB_PORT"] = str(PORT)
    env["AIHUB_MYSQL_DB"] = "test_mysql"
    proc = subprocess.Popen(
        [str(REPO_ROOT / ".venv" / "Scripts" / "python.exe"), "-m", "app.main"],
        cwd=REPO_ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        wait_health()
        print("[1/8] health ok")

        status = http("GET", "/api/status")
        assert status["db"]["ok"], f"数据库探活失败: {status}"
        print(f"[2/8] status ok: v{status['version']} db={status['db']['database']}")

        http("POST", "/api/events", {
            "source": "OTHER", "eventType": "TASK_COMPLETED",
            "externalTaskId": "e2e-smoke", "title": "e2e 冒烟任务",
            "projectPath": str(REPO_ROOT),
        })
        queue = http("GET", "/api/tasks?view=queue")["tasks"]
        assert len(queue) == 1, f"队列应为 1，实际 {len(queue)}"
        task = queue[0]
        print(f"[3/8] event -> queue ok: #{task['id']}")

        events = http("GET", f"/api/tasks/{task['id']}/events")["events"]
        assert events and events[0]["eventType"] == "TASK_COMPLETED"
        assert isinstance(events[0]["payload"], dict) and events[0]["occurredAt"]
        print("[4/8] timeline contract ok (camelCase + payload object)")

        read_all = http("POST", "/api/tasks/read-all")
        assert read_all["count"] == 1
        assert http("GET", "/api/tasks?view=queue")["tasks"] == []
        assert len(http("GET", "/api/tasks?view=history")["tasks"]) == 1
        print("[5/8] read-all ok（未读清零，历史 +1）")

        cleared = http("DELETE", "/api/tasks")
        assert cleared["deleted"] >= 1
        print("[6/8] clear-all ok")

        # WebSocket：建连后触发事件，应收到 task_changed 广播
        async def ws_roundtrip() -> dict:
            async with websockets.connect(f"ws://127.0.0.1:{PORT}/ws/tasks") as ws:
                http("POST", "/api/events", {
                    "source": "CHATGPT", "eventType": "TASK_NEEDS_INPUT",
                    "externalTaskId": "e2e-ws", "title": "WS 广播冒烟",
                })
                return json.loads(await asyncio.wait_for(ws.recv(), timeout=10))

        msg = asyncio.run(ws_roundtrip())
        assert msg["type"] == "task_changed" and msg["eventType"] == "TASK_NEEDS_INPUT"
        print("[7/8] websocket broadcast ok")

        http("DELETE", "/api/tasks")
        print("[8/8] cleanup ok")
        print("\nE2E SMOKE PASS")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
