"""Claude Code Hook → AI Task Hub 统一事件适配器。

Claude Code 在钩子触发时通过 stdin 传入 JSON 载荷，本脚本将其转换为
统一事件（shared/event_schema.json）并 POST 到本地事件服务。

配置方式见同目录 settings.example.json，合并到 ~/.claude/settings.json。
钩子脚本必须永不阻塞 Claude Code：任何异常都静默退出 0。

标题策略：UserPromptSubmit 携带 prompt 时直接作为标题并写入会话缓存；
Notification/Stop 不带 prompt，依次回退到「会话缓存 → 通知消息 → 项目目录名」，
保证队列中的任务始终有可读标题。
"""

import json
import sys
import urllib.request
from pathlib import Path

API_URL = "http://127.0.0.1:17891/api/events"
TIMEOUT_SEC = 2
MAX_TITLE_LEN = 60

CACHE_PATH = Path(__file__).resolve().parent / "session_titles.json"
CACHE_MAX = 100

# Claude Code 钩子事件 → 统一事件类型
HOOK_EVENT_MAP = {
    "UserPromptSubmit": "TASK_STARTED",
    "Notification": "TASK_NEEDS_INPUT",
    "Stop": "TASK_COMPLETED",
}


def _truncate(text: str) -> str:
    text = text.strip().replace("\n", " ")
    return text[:MAX_TITLE_LEN] + ("…" if len(text) > MAX_TITLE_LEN else "")


def _load_cache() -> dict:
    try:
        return json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _save_title(session_id: str, title: str) -> None:
    if not session_id or not title:
        return
    try:
        cache = _load_cache()
        cache[session_id] = title
        # 只保留最近 CACHE_MAX 条，防止无限增长
        items = list(cache.items())[-CACHE_MAX:]
        CACHE_PATH.write_text(json.dumps(dict(items), ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass


def _cached_title(session_id: str) -> str | None:
    if not session_id:
        return None
    return _load_cache().get(session_id)


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
        prompt = (payload.get("prompt") or "").strip()
        if prompt:
            title = _truncate(prompt)
            _save_title(session_id, title)
    elif hook_event == "Notification":
        message = (payload.get("message") or "").strip()
        content_preview = message or "Claude Code 等待确认"
        title = _cached_title(session_id)
        if not title and message:
            title = _truncate(message)
    elif hook_event == "Stop":
        title = _cached_title(session_id)
        if not title and cwd:
            title = f"{Path(cwd).name} 会话"

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
        # Claude Code 以 UTF-8 写入 stdin；Windows 上 Python 默认按控制台代码页（GBK）解码会导致中文乱码
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
        # 桌面端未启动等任何失败都不允许影响 Claude Code 主流程
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
