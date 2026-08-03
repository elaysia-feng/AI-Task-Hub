"""Claude Code Hook → AI Task Hub 统一事件适配器。

Claude Code 在钩子触发时通过 stdin 传入 JSON 载荷，本脚本将其转换为
统一事件（shared/event_schema.json）并 POST 到本地事件服务。

配置方式见同目录 settings.example.json，合并到 ~/.claude/settings.json。
钩子脚本必须永不阻塞 Claude Code：任何异常都静默退出 0。

标题策略（对齐 Codex 的「用户提问作主题」）：
- UserPromptSubmit：把人类可读的 prompt 提炼为标题并写入会话缓存；
  跳过 OMC `<task-notification>`、日志行、代码片段等噪声，不覆盖已有好标题。
- Notification/Stop：优先用会话缓存；否则回退到「项目目录名 + 会话」。
"""

import json
import os
import re
import sys
import urllib.request
from pathlib import Path

# 端口允许用 AIHUB_PORT 覆盖（冒烟测试并行实例、端口冲突场景）
API_URL = f"http://127.0.0.1:{int(os.environ.get('AIHUB_PORT', '17891'))}/api/events"
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

_NOISE_PREFIXES = (
    "<task-notification",
    "<task-",
    "<teammate",
    "<agent-",
    "system:",
)
_LOG_LINE_RE = re.compile(r"^\d{1,2}:\d{2}:\d{2}\b")
_CODE_START_RE = re.compile(
    r"^(if\s*\(|function\s|const\s|let\s|var\s|import\s|from\s|def\s|class\s|export\s|#include)"
)
_XML_TAG_RE = re.compile(
    r"<(summary|title|message|description|subject|task|content)[^>]*>(.*?)</\1>",
    re.I | re.S,
)
_GENERIC_WAIT_RE = re.compile(
    r"waiting for your input|needs your permission|claude code 等待",
    re.I,
)


def _truncate(text: str) -> str:
    text = text.strip().replace("\n", " ")
    return text[:MAX_TITLE_LEN] + ("…" if len(text) > MAX_TITLE_LEN else "")


def _is_noise(text: str) -> bool:
    """判断文本是否不适合作为任务主题（系统注入 / 日志 / 代码）。"""
    t = text.strip()
    if not t:
        return True
    low = t.lower()
    if any(low.startswith(p) for p in _NOISE_PREFIXES):
        return True
    if _LOG_LINE_RE.match(t) or "[vite]" in t[:80].lower():
        return True
    if _CODE_START_RE.match(t):
        return True
    if _GENERIC_WAIT_RE.search(t) and len(t) < 80:
        return True
    # 整段几乎都是 XML/标签
    if t.count("<") >= 2 and t.count(">") >= 2 and len(re.sub(r"<[^>]+>", "", t).strip()) < 8:
        return True
    return False


def _extract_from_markup(text: str) -> str | None:
    """从 OMC task-notification 等 XML 里抠出可读摘要。"""
    for match in _XML_TAG_RE.finditer(text):
        inner = re.sub(r"<[^>]+>", " ", match.group(2))
        inner = re.sub(r"\s+", " ", inner).strip()
        if inner and not _is_noise(inner) and len(inner) >= 4:
            return inner
    return None


def _project_title(cwd: str) -> str:
    name = Path(cwd).name if cwd else "Claude"
    return f"{name} 会话"


def _human_title(prompt: str) -> str | None:
    """把 prompt 提炼成人类可读主题；噪声则返回 None（不覆盖缓存）。"""
    prompt = (prompt or "").strip()
    if not prompt:
        return None
    extracted = _extract_from_markup(prompt)
    if extracted:
        return _truncate(extracted)
    if _is_noise(prompt):
        return None
    return _truncate(prompt)


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
        human = _human_title(prompt)
        if human:
            title = human
            _save_title(session_id, title)
        else:
            # 噪声 prompt：保留已有主题；没有则用项目名，不把噪声写入缓存
            title = _cached_title(session_id) or _project_title(cwd)
            if prompt:
                content_preview = _truncate(prompt)
    elif hook_event == "Notification":
        message = (payload.get("message") or "").strip()
        content_preview = message or "Claude Code 等待确认"
        title = _cached_title(session_id)
        if not title:
            # 绝不把「waiting for your input」这类通用句当主题
            title = _project_title(cwd)
    elif hook_event == "Stop":
        title = _cached_title(session_id) or _project_title(cwd)

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
