"""ZCode 钩子 → AI Task Hub 统一事件适配器。

ZCode（智谱 Z.ai 的开源编程代理）在 `~/.zcode/cli/config.json` 的
`hooks.events.<Event>` 下登记进程型钩子，触发时以 JSON 形式把事件载荷写入
本脚本的 stdin。本脚本把载荷转换为统一事件（shared/event_schema.json）
并 POST 到本地事件服务。

配置形态（由 AI Task Hub 的「一键接入」写入，勿手工改动）：

    {
      "hooks": {
        "enabled": true,
        "events": {
          "UserPromptSubmit": [
            {"hooks": [{"type": "process", "command": "<python>",
                        "args": ["<zcode_adapter.py>"], "timeoutMs": 8000}]}
          ],
          "PermissionRequest": [...],
          "Stop": [...]
        }
      }
    }

注意：
- ZCode 的配置钩子默认关闭，`hooks.enabled` 必须为 true 才会执行。
- ZCode 只支持 7 个事件，没有 Notification；「等待用户确认」用
  PermissionRequest 表达。
- 载荷字段可能同时出现 camelCase 与 snake_case 两种写法，这里全部兼容。

钩子脚本必须永不阻塞 ZCode：任何异常都静默退出 0，且不向 stdout 写内容
（stdout 会被 ZCode 当作钩子应答解析）。
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
MAX_PREVIEW_LEN = 200

CACHE_PATH = Path(__file__).resolve().parent / "session_titles.json"
CACHE_MAX = 100

# ZCode 钩子事件 → 统一事件类型（只登记对任务生命周期有意义的三个）
HOOK_EVENT_MAP = {
    "UserPromptSubmit": "TASK_STARTED",
    "PermissionRequest": "TASK_NEEDS_INPUT",
    "Stop": "TASK_COMPLETED",
}

# 载荷字段的多种写法：ZCode 兼容层可能同时给出 camelCase 与 snake_case
_EVENT_KEYS = ("hook_event_name", "hookEventName", "event_name", "eventName", "event", "type")
_SESSION_KEYS = ("session_id", "sessionId")
_CWD_KEYS = ("cwd", "workingDirectory", "working_directory", "workspace", "projectPath", "project_path")
_PROMPT_KEYS = ("prompt", "user_prompt", "userPrompt", "userPromptText")
_TOOL_KEYS = ("tool_name", "toolName")
_TURN_KEYS = ("turn_id", "turnId")

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


def _truncate(text: str, limit: int) -> str:
    """合并空白并截断显示文本。"""
    text = (text or "").strip().replace("\n", " ")
    return text[:limit] + ("…" if len(text) > limit else "")


def _pick(payload: dict, keys: tuple[str, ...]) -> str | None:
    """按候选字段名顺序取第一个非空字符串值。"""
    for key in keys:
        value = payload.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return None


def _is_noise(text: str) -> bool:
    """判断文本不适合作为任务主题（系统注入 / 日志 / 代码）。"""
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
    # 整段几乎都是 XML/标签
    if t.count("<") >= 2 and t.count(">") >= 2 and len(re.sub(r"<[^>]+>", "", t).strip()) < 8:
        return True
    return False


def _extract_from_markup(text: str) -> str | None:
    """从 XML 片段里抠出可读摘要。"""
    for match in _XML_TAG_RE.finditer(text):
        inner = re.sub(r"<[^>]+>", " ", match.group(2))
        inner = re.sub(r"\s+", " ", inner).strip()
        if inner and not _is_noise(inner) and len(inner) >= 4:
            return inner
    return None


def _human_title(prompt: str) -> str | None:
    """把用户提问提炼成人类可读主题；噪声则返回 None（不覆盖缓存）。"""
    prompt = (prompt or "").strip()
    if not prompt:
        return None
    extracted = _extract_from_markup(prompt)
    if extracted:
        return _truncate(extracted, MAX_TITLE_LEN)
    if _is_noise(prompt):
        return None
    return _truncate(prompt, MAX_TITLE_LEN)


def _project_title(cwd: str) -> str:
    name = Path(cwd).name if cwd else "ZCode"
    return f"{name} 会话"


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
    """把 ZCode 钩子载荷转换为统一事件；无关事件返回 None。"""
    hook_event = _pick(payload, _EVENT_KEYS)
    event_type = HOOK_EVENT_MAP.get(hook_event) if hook_event else None
    if event_type is None:
        return None

    session_id = _pick(payload, _SESSION_KEYS) or ""
    cwd = _pick(payload, _CWD_KEYS) or ""
    turn_id = _pick(payload, _TURN_KEYS)

    title = None
    content_preview = None
    if hook_event == "UserPromptSubmit":
        prompt = _pick(payload, _PROMPT_KEYS) or ""
        human = _human_title(prompt)
        if human:
            title = human
            _save_title(session_id, title)
        else:
            # 噪声 prompt：保留已有主题；没有则用项目名
            title = _cached_title(session_id) or _project_title(cwd)
            if prompt:
                content_preview = _truncate(prompt, MAX_PREVIEW_LEN)
    elif hook_event == "PermissionRequest":
        tool = _pick(payload, _TOOL_KEYS)
        title = _cached_title(session_id) or _project_title(cwd)
        content_preview = _truncate(f"ZCode 等待确认：{tool}" if tool else "ZCode 等待确认", MAX_PREVIEW_LEN)
    elif hook_event == "Stop":
        title = _cached_title(session_id) or _project_title(cwd)

    return {
        "source": "ZCODE",
        "eventType": event_type,
        "externalTaskId": session_id or None,
        "turnId": turn_id,
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
        # ZCode 以 UTF-8 写入 stdin；Windows 上 Python 默认按控制台代码页（GBK）解码会导致中文乱码
        try:
            sys.stdin.reconfigure(encoding="utf-8")
        except Exception:
            pass
        raw = sys.stdin.read()
        payload = json.loads(raw) if raw.strip() else {}
        event = build_event(payload) if isinstance(payload, dict) else None
        if event:
            post_event(event)
    except Exception:
        # 事件服务未启动等任何失败都不允许影响 ZCode 主流程
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
