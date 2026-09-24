"""Codex 用户提交问题时上报 TASK_STARTED，保证任务在模型响应前进入运行中。"""

import json
import os
import sys
import urllib.request

from event_converter import MAX_TITLE_LEN, is_internal_title_prompt, truncate

API_URL = f"http://127.0.0.1:{int(os.environ.get('AIHUB_PORT', '17891'))}/api/events"
TIMEOUT_SEC = 2
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def build_event(payload: dict) -> dict | None:
    """把 Codex UserPromptSubmit 载荷转成稳定会话 ID 的开始事件。

    Args:
        payload: Codex 命令 hook 通过标准输入传入的 JSON 对象。

    Returns:
        TASK_STARTED 事件；事件名不匹配或载荷缺少 session_id 时返回 None。
    """
    if payload.get("hook_event_name") != "UserPromptSubmit":
        return None

    session_id = payload.get("session_id")
    if not isinstance(session_id, str) or not session_id:
        return None

    prompt = payload.get("prompt")
    if not isinstance(prompt, str):
        prompt = ""
    if is_internal_title_prompt(prompt):
        return None
    return {
        "source": "CODEX",
        "eventType": "TASK_STARTED",
        "externalTaskId": session_id,
        "turnId": payload.get("turn_id") if isinstance(payload.get("turn_id"), str) else None,
        "title": truncate(prompt, MAX_TITLE_LEN) if prompt.strip() else None,
        "projectPath": payload.get("cwd") or None,
        "openTarget": "terminal",
    }


def post_event(event: dict) -> None:
    """同步提交事件到本地服务，确保模型请求前先写入运行状态。

    Args:
        event: 统一事件 API 接收的 JSON 对象。
    """
    body = json.dumps(event, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        API_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with _opener.open(request, timeout=TIMEOUT_SEC):
        pass


def main() -> None:
    """读取 Codex hook 输入并上报；任何接入错误都不阻止用户提交问题。"""
    try:
        try:
            sys.stdin.reconfigure(encoding="utf-8")
        except Exception:
            pass
        raw = sys.stdin.read()
        payload = json.loads(raw) if raw.strip() else {}
        event = build_event(payload)
        if event:
            post_event(event)
    except Exception:
        # 接入失败不得阻塞用户向 Codex 提交问题。
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
