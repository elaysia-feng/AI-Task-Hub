"""Codex notify 链式适配器：上报 AI Task Hub 的同时转发给原始 notify 目标。

Codex 只支持单个 notify 命令，而本机已被 Codex 桌面端占用。
config.toml 中的 notify 指向本脚本后，事件流向：
    codex → notify_chain.py <payload>
              ├── POST /api/events（AI Task Hub）
              └── 原命令（forward_target.json，Codex 桌面通知）

任何异常静默退出 0，绝不阻塞 Codex。
"""

import json
import os
import subprocess
import sys
import urllib.request
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import codex_notify_to_event  # noqa: E402

API_URL = "http://127.0.0.1:17891/api/events"
TIMEOUT_SEC = 2
FORWARD_TARGET_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "forward_target.json")
DEBUG_LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notify_debug.log")


# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def debug_log(entry: dict) -> None:
    """排障日志：记录每次 notify 触发的载荷与处理结果，任何失败静默。"""
    try:
        entry["ts"] = datetime.now().isoformat(timespec="seconds")
        with open(DEBUG_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception:
        pass


def post_event(event: dict) -> str:
    try:
        body = json.dumps(event).encode("utf-8")
        request = urllib.request.Request(
            API_URL,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with _opener.open(request, timeout=TIMEOUT_SEC):
            pass
        return "ok"
    except Exception as exc:
        return f"error: {exc}"


def forward(payload_json: str) -> None:
    """把原始载荷转发给被接管的官方 notify 命令。"""
    try:
        with open(FORWARD_TARGET_PATH, encoding="utf-8") as f:
            target = json.load(f).get("command")
        if not target or not os.path.exists(target[0]):
            return
        subprocess.run(
            [*target, payload_json],
            timeout=10,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        pass


def main() -> None:
    debug_log({"stage": "invoked", "argv": sys.argv[1:], "cwd": os.getcwd()})
    try:
        payload_json = sys.argv[-1] if len(sys.argv) > 1 else "{}"
        payload = json.loads(payload_json)
    except Exception as exc:
        debug_log({"stage": "parse_failed", "error": str(exc), "argv": sys.argv[1:]})
        sys.exit(0)

    try:
        event = codex_notify_to_event(payload, cwd=os.getcwd())
        if event:
            result = post_event(event)
            debug_log({"stage": "posted", "result": result, "payload_type": payload.get("type")})
        else:
            debug_log({"stage": "skipped", "reason": "converter returned None", "payload_type": payload.get("type"), "payload_keys": sorted(payload.keys())})
    except Exception as exc:
        debug_log({"stage": "error", "error": str(exc)})

    forward(payload_json)
    sys.exit(0)


if __name__ == "__main__":
    main()
