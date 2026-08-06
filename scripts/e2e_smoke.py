"""端到端冒烟：独立端口后端实例上跑完整事件生命周期。

用法：
    .venv/Scripts/python.exe scripts/e2e_smoke.py              # 默认 MySQL（ai_task_hub_test）
    AIHUB_DB_BACKEND=sqlite .venv/Scripts/python.exe scripts/e2e_smoke.py

- 拉起 AIHUB_PORT=17899 的隔离后端（不碰正式库/正式端口）
- mysql：AIHUB_MYSQL_DB=ai_task_hub_test；sqlite：临时库文件，无需 MySQL
- 断言：health → status → 事件 → 队列 → 时间线契约 → read-all → clear → WebSocket 广播
- 结束自动清理（清空测试数据、终止子进程）
"""

import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import websockets

PORT = 17899
BASE = f"http://127.0.0.1:{PORT}"
REPO_ROOT = Path(__file__).resolve().parent.parent


def _python_exe() -> Path:
    if sys.platform == "win32":
        return REPO_ROOT / ".venv" / "Scripts" / "python.exe"
    return REPO_ROOT / ".venv" / "bin" / "python"


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
    last_err: Exception | None = None
    while time.time() < deadline:
        try:
            if http("GET", "/api/health")["status"] == "ok":
                return
        except (OSError, ValueError, KeyError) as exc:
            # 启动中的瞬态失败（连接拒绝 / 非 JSON / 缺 status）都预期，记下最后一次用于报错
            last_err = exc
            time.sleep(0.5)
    raise RuntimeError(f"后端冒烟实例启动超时: {last_err}")


def main() -> None:
    backend = os.environ.get("AIHUB_DB_BACKEND", "auto").strip().lower()
    env = os.environ.copy()
    env["AIHUB_PORT"] = str(PORT)
    sqlite_dir: Path | None = None
    if backend == "sqlite":
        # sqlite 分支：临时库文件，无需 MySQL
        sqlite_dir = Path(tempfile.mkdtemp(prefix="aihub-e2e-sqlite-"))
        env["AIHUB_DB_BACKEND"] = "sqlite"
        env["AIHUB_SQLITE_PATH"] = str(sqlite_dir / "e2e.sqlite")
    else:
        # 默认 / auto / mysql：MySQL 测试库（保留原有路径；auto 不静默降级默认库路径）
        env["AIHUB_DB_BACKEND"] = "mysql"
        env["AIHUB_MYSQL_DB"] = "ai_task_hub_test"
    proc = subprocess.Popen(
        [str(_python_exe()), "-m", "app.main"],
        cwd=REPO_ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        wait_health()
        print("[1/9] health ok")

        status = http("GET", "/api/status")
        assert status["db"]["ok"], f"数据库探活失败: {status}"
        print(f"[2/9] status ok: v{status['version']} backend={status['db']['backend']} db={status['db']['database']}")

        # 前次运行若中途失败会残留任务，先清空保证可重复执行
        http("DELETE", "/api/tasks?confirm=true")
        assert http("GET", "/api/tasks?view=queue")["tasks"] == []
        assert http("GET", "/api/tasks?view=history")["tasks"] == []

        http("POST", "/api/events", {
            "source": "OTHER", "eventType": "TASK_COMPLETED",
            "externalTaskId": "e2e-smoke", "title": "e2e 冒烟任务",
            "projectPath": str(REPO_ROOT),
        })
        queue = http("GET", "/api/tasks?view=queue")["tasks"]
        assert len(queue) == 1, f"队列应为 1，实际 {len(queue)}"
        task = queue[0]
        print(f"[3/9] event -> queue ok: #{task['id']}")

        events = http("GET", f"/api/tasks/{task['id']}/events")["events"]
        assert events and events[0]["eventType"] == "TASK_COMPLETED"
        assert isinstance(events[0]["payload"], dict) and events[0]["occurredAt"]
        print("[4/9] timeline contract ok (camelCase + payload object)")

        read_all = http("POST", "/api/tasks/read-all")
        assert read_all["count"] == 1
        assert http("GET", "/api/tasks?view=queue")["tasks"] == []
        assert len(http("GET", "/api/tasks?view=history")["tasks"]) == 1
        print("[5/9] read-all ok（未读清零，历史 +1）")

        cleared = http("DELETE", "/api/tasks?confirm=true")
        assert cleared["deleted"] >= 1
        print("[6/9] clear-all ok")

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
        print("[7/9] websocket task_changed broadcast ok")

        # WebSocket：tasks_cleared 广播
        async def ws_cleared() -> dict:
            async with websockets.connect(f"ws://127.0.0.1:{PORT}/ws/tasks") as ws:
                res = http("DELETE", "/api/tasks?confirm=true")
                assert res["deleted"] >= 0
                return json.loads(await asyncio.wait_for(ws.recv(), timeout=10))

        msg = asyncio.run(ws_cleared())
        assert msg["type"] == "tasks_cleared"
        print("[8/9] websocket tasks_cleared broadcast ok")

        res = http("DELETE", "/api/tasks?confirm=true")
        assert res["deleted"] >= 0
        print("[cleanup] task cleanup ok")
        print("\nE2E SMOKE PASS")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        if sqlite_dir is not None:
            shutil.rmtree(sqlite_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
