"""Claude Code Hook → AI Task Hub 统一事件适配器。

Claude Code 在钩子触发时通过 stdin 传入 JSON 载荷，本脚本将其转换为
统一事件（shared/event_schema.json）并 POST 到本地事件服务。

配置方式见同目录 settings.example.json，合并到 ~/.claude/settings.json。
钩子脚本必须永不阻塞 Claude Code：任何异常都静默退出 0。
"""

import json
import sys
import urllib.request

API_URL = "http://127.0.0.1:17891/api/events"
TIMEOUT_SEC = 2
MAX_TITLE_LEN = 60

# Claude Code 钩子事件 → 统一事件类型
HOOK_EVENT_MAP = {
    "UserPromptSubmit": "TASK_STARTED",
    "Notification": "TASK_NEEDS_INPUT",
    "Stop": "TASK_COMPLETED",
}


def build_event(payload: dict) -> dict | None:
    hook_event = payload.get("hook_event_name")
    event_type = HOOK_EVENT_MAP.get(hook_event)
    if event_type is None:
        return None

    session_id = payload.get("session_id") or ""
    cwd = payload.get("cwd") or ""

    title = None
    content_preview = None
    if hook_event == "UserPromptSubmit":
        prompt = (payload.get("prompt") or "").strip().replace("\n", " ")
        if prompt:
            title = prompt[:MAX_TITLE_LEN] + ("…" if len(prompt) > MAX_TITLE_LEN else "")
    elif hook_event == "Notification":
        content_preview = payload.get("message") or "Claude Code 等待确认"

    return {
        "source": "CLAUDE_CODE",
        "eventType": event_type,
        "externalTaskId": session_id or None,
        "title": title,
        "contentPreview": content_preview,
        "projectPath": cwd or None,
        "openTarget": "terminal",
    }


# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连，否则会被代理拦截
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def post_event(event: dict) -> None:
    body = json.dumps(event).encode("utf-8")
    request = urllib.request.Request(
        API_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with _opener.open(request, timeout=TIMEOUT_SEC):
        pass


def main() -> None:
    try:
        raw = sys.stdin.read()
        payload = json.loads(raw) if raw.strip() else {}
        event = build_event(payload)
        if event:
            post_event(event)
    except Exception:
        # 桌面端未启动等任何失败都不允许影响 Claude Code 主流程
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
