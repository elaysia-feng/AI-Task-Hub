"""Codex 载荷 → 统一事件 的转换逻辑（notify.py 与 launcher.py 共用）。

Codex notify 钩子传入的 JSON 形如：
{
  "type": "agent-turn-complete",
  "session-id": "…",
  "input-messages": ["用户消息…"],
  "last-assistant-message": "最后一条助手回复…"
}
"""

MAX_TITLE_LEN = 60
MAX_PREVIEW_LEN = 200


def truncate(text: str, limit: int) -> str:
    text = text.strip().replace("\n", " ")
    return text[:limit] + ("…" if len(text) > limit else "")


def codex_notify_to_event(payload: dict, cwd: str | None) -> dict | None:
    """Codex turn 完成通知 → TASK_COMPLETED 统一事件。"""
    if payload.get("type") != "agent-turn-complete":
        return None

    input_messages = payload.get("input-messages") or []
    if not input_messages:
        return None
    title = truncate(input_messages[0], MAX_TITLE_LEN)
    last_message = payload.get("last-assistant-message") or ""

    return {
        "source": "CODEX",
        "eventType": "TASK_COMPLETED",
        # 优先会话级 ID（同一会话多轮合并为一个任务），turn-id 每轮都变仅作兜底
        "externalTaskId": payload.get("session-id") or payload.get("thread-id") or payload.get("turn-id"),
        "title": title,
        "contentPreview": truncate(last_message, MAX_PREVIEW_LEN) if last_message else None,
        "projectPath": cwd,
        "openTarget": "terminal",
    }


def launcher_event(event_type: str, external_id: str, cwd: str, title: str | None) -> dict:
    """launcher 包装模式下的事件构造。"""
    return {
        "source": "CODEX",
        "eventType": event_type,
        "externalTaskId": external_id,
        "title": title,
        "projectPath": cwd,
        "openTarget": "terminal",
    }
