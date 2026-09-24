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
_INTERNAL_TITLE_PROMPT_PREFIXES = (
    "generate a concise, single-line task title",
    "write a brief catch-up for a user returning to this codex",
)


def truncate(text: str, limit: int) -> str:
    """合并换行并截断显示文本。

    Args:
        text: 原始文本。
        limit: 最大保留字符数。

    Returns:
        截断后的文本；超长时以省略号结尾。
    """
    text = text.strip().replace("\n", " ")
    return text[:limit] + ("…" if len(text) > limit else "")


def is_internal_title_prompt(text: str) -> bool:
    """识别 Codex 内部生成标题或补充上下文的提示。"""
    return text.strip().lower().startswith(_INTERNAL_TITLE_PROMPT_PREFIXES)


def codex_notify_to_event(payload: dict, cwd: str | None) -> dict | None:
    """Codex turn 完成通知转为统一事件；缺少稳定会话 ID 时跳过。

    Args:
        payload: Codex notify 传入的完成事件。
        cwd: Codex 工作目录，可为空。

    Returns:
        统一完成事件；载荷不是完成事件或没有稳定会话 ID 时返回 None。
    """
    if payload.get("type") != "agent-turn-complete":
        return None

    # turn-id 每轮都会变化，不能用它代表跨轮次的同一会话。
    session_id = payload.get("session-id") or payload.get("thread-id")
    if not isinstance(session_id, str) or not session_id:
        return None

    input_messages = payload.get("input-messages")
    if not isinstance(input_messages, list) or not input_messages:
        return None
    # 过滤 Codex 在用户输入后附加的内部标题请求，避免它成为任务名。
    user_messages = [
        message for message in input_messages
        if isinstance(message, str)
        and message.strip()
        and not is_internal_title_prompt(message)
    ]
    if not user_messages:
        return None
    title = truncate(user_messages[-1], MAX_TITLE_LEN)
    last_message = payload.get("last-assistant-message") or ""
    if not isinstance(last_message, str):
        last_message = ""

    return {
        "source": "CODEX",
        "eventType": "TASK_COMPLETED",
        "externalTaskId": session_id,
        "turnId": payload.get("turn-id") if isinstance(payload.get("turn-id"), str) else None,
        "title": title,
        "contentPreview": truncate(last_message, MAX_PREVIEW_LEN) if last_message else None,
        "projectPath": cwd,
        "openTarget": "terminal",
    }


def launcher_event(event_type: str, external_id: str, cwd: str, title: str | None) -> dict:
    """构造 launcher 包装模式下的统一事件。

    Args:
        event_type: 统一事件类型。
        external_id: launcher 生成的外部任务 ID。
        cwd: Codex 工作目录。
        title: 任务标题，可为空。

    Returns:
        符合事件 API 字段约定的字典。
    """
    return {
        "source": "CODEX",
        "eventType": event_type,
        "externalTaskId": external_id,
        "title": title,
        "projectPath": cwd,
        "openTarget": "terminal",
    }
