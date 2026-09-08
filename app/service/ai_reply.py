"""AI 答复提取：按来源从本地会话记录读取任务对应的最终 AI 答复。

- CLAUDE_CODE：读 `~/.claude/projects/<encoded-cwd>/<session_id>.jsonl`，取最后一个
  assistant 文本块（忽略 thinking / tool_use 与尾部 last-prompt / custom-title 等元数据行）。
- CODEX：在 `~/.codex/sessions/**/rollout-*.jsonl` 中按文件名 UUID == session_id 定位，
  取最后一条 assistant `output_text`，优先 `payload.phase == "final_answer"`。
- CHATGPT：会话在浏览器、服务器无本地记录；从最近完成事件的 `payload.replyText` 取
  （由浏览器扩展在 TASK_COMPLETED 事件里随事件上报）。

任何缺失 / 读取失败都返回 (None, 友好错误文案)，绝不抛异常——展示是尽力而为，
不能因为会话记录不可读而阻塞任务详情。读取是阻塞 IO，但端点用 sync def 由 FastAPI
跑在线程池，不会卡事件循环。
"""

import json
import logging
import os
import re
import threading
import time
from pathlib import Path
from typing import Optional

from app.model.task import Task

logger = logging.getLogger(__name__)

# 单行 JSONL 超长跳过（防巨型 tool_result / 内嵌资源行把解析拖垮）
_MAX_LINE_BYTES = 2 * 1024 * 1024
# 最终答复文本上限：超出截断，避免把整段会话当答复展示
_MAX_REPLY_CHARS = 200_000
# 进程内 TTL 缓存：详情面板重复打开免重复读会话文件。
# 超长答复不进缓存，避免少数详情页占用大量常驻 Python 堆内存。
_CACHE_TTL_SEC = 30.0
_CACHE_MAX = 16
_CACHE_MAX_CONTENT_CHARS = 16_384

# 中文友好错误（展示在详情面板的 muted 提示里）
ERR_MISSING_ID = "任务缺少会话标识，无法定位会话记录"
ERR_MISSING_PATH = "任务缺少项目路径，无法定位会话记录"
ERR_INVALID_ID = "会话标识非法，已拒绝读取"
ERR_NO_TRANSCRIPT = "会话记录不存在"
ERR_NO_REPLY = "会话记录中未找到最终答复"
ERR_REPLY_TOO_LONG = "最终答复过长，无法完整读取"
ERR_READ_FAILED = "会话记录读取失败"
ERR_UNSUPPORTED = "该来源无本地会话记录"

# 会话 ID 白名单：平台生成的 UUID / 短 id（字母数字 + `-`/`_`）。
# 拒绝路径分隔符与 `..` 等遍历序列：external_task_id 来自事件入口，可被伪造，
# 直接拼进路径会逃逸 projects 目录读任意文件（Path Traversal）。
_SAFE_SESSION_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def _safe_session_id(session_id: str) -> bool:
    return bool(_SAFE_SESSION_RE.fullmatch(session_id))


def encode_claude_cwd(cwd: str) -> str:
    """Claude Code 项目目录编码：分隔符 `\\`、`/` 与盘符冒号 `:` 全部折叠为单个 `-`。

    实测磁盘目录（如 `~/.claude/projects/C--Users-seele-Desktop-AI-Task-Hub`）验证：
    `C:\\Users\\seele\\Desktop\\AI-Task-Hub` → `C--Users-seele-Desktop-AI-Task-Hub`
    （盘符冒号与首分隔符各成一个 `-`）。注意不是冒号→`--`，那是易错点。

    Args:
        cwd: Claude Code 会话所属的原始项目路径。

    Returns:
        可作为 Claude `projects` 子目录名的编码路径。
    """
    return cwd.replace("\\", "-").replace("/", "-").replace(":", "-")


def claude_config_dir() -> Path:
    """返回 Claude Code 配置根目录。

    Returns:
        环境变量指定或用户主目录下的 Claude 配置路径。
    """
    return Path(os.environ.get("CLAUDE_CONFIG_DIR") or Path.home() / ".claude")


def codex_home_dir() -> Path:
    """返回 Codex 配置根目录。

    Returns:
        环境变量指定或用户主目录下的 Codex 配置路径。
    """
    return Path(os.environ.get("CODEX_HOME") or Path.home() / ".codex")


def claude_transcript_path(cwd: str, session_id: str) -> Path:
    """根据项目路径和会话标识生成 Claude 会话记录路径。

    Args:
        cwd: Claude Code 会话所属的项目路径。
        session_id: Claude Code 会话标识。

    Returns:
        对应 JSONL 会话记录的预期路径，不保证文件存在。
    """
    return claude_config_dir() / "projects" / encode_claude_cwd(cwd) / f"{session_id}.jsonl"


def _cap(text: str) -> str:
    if len(text) > _MAX_REPLY_CHARS:
        return text[:_MAX_REPLY_CHARS] + "…(截断)"
    return text


def _text_blocks(content) -> list[str]:
    """从 message.content 提取可展示文本块：`{type:"text"}`（Claude）或 `{type:"output_text"}`（Codex）。

    忽略 thinking / tool_use / tool_result 等块；content 为裸字符串时原样返回。
    """
    if isinstance(content, str):
        return [content] if content.strip() else []
    if isinstance(content, list):
        out = []
        for block in content:
            if isinstance(block, dict) and block.get("type") in ("text", "output_text"):
                text = block.get("text")
                if text:
                    out.append(text)
        return out
    return []


def _iter_lines_reverse(path: Path):
    """从文件尾部向前逐行产出（内存安全：64KB 分块，超大文件不整体载入）。

    产出的顺序是「最新一行在前」。文件尾部可能的半行（活跃会话写入中）会作为坏 JSON
    被调用方跳过，不影响取数。

    用二进制模式 + 手动解码：文本模式下 seek（字节偏移）与 read（字符数）在 `\r\n`
    换行翻译后错位，跨块边界会多/丢字符。二进制读对 `\n` 与 `\r\n` 行尾都正确。
    """
    chunk_size = 64 * 1024
    with path.open("rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        pos = size
        pending = b""  # 尚未拼成完整行的尾部残片
        while pos > 0:
            start = max(0, pos - chunk_size)
            f.seek(start)
            data = f.read(pos - start)
            pos = start
            chunk = data + pending
            pending = b""
            lines = chunk.split(b"\n")
            # lines[0] 的开头可能延续到更早的块（不完整），只处理 lines[1:]
            for line in reversed(lines[1:]):
                yield line.decode("utf-8", errors="replace").rstrip("\r")
            pending = lines[0]
        if pending:
            yield pending.decode("utf-8", errors="replace").rstrip("\r")


def read_claude_reply(cwd: str, session_id: str) -> tuple[Optional[str], Optional[str]]:
    """读取 Claude Code 会话的最后一条可展示答复。

    Args:
        cwd: Claude Code 会话所属的项目路径。
        session_id: Claude Code 会话标识，必须满足安全白名单。

    Returns:
        二元组；成功时为（答复文本，None），失败时为（None，中文错误说明）。
    """
    # 1. 先校验不可信的会话定位参数，阻止路径逃逸。
    if not session_id:
        return (None, ERR_MISSING_ID)
    if not _safe_session_id(session_id):
        return (None, ERR_INVALID_ID)
    encoded = encode_claude_cwd(cwd)
    # encode 后分隔符已替换，理论上不会产生 `..`；但 `cwd="."/".."` 会原样保留，
    # 落到 projects 层之外。防御性拒绝（伪造事件的 projectPath 不可信）。
    if encoded in ("", ".", ".."):
        return (None, ERR_MISSING_PATH)
    path = claude_transcript_path(cwd, session_id)
    if not path.is_file():
        return (None, ERR_NO_TRANSCRIPT)
    skipped_oversized = False  # 逆序扫描中是否有更晚（更新）的行因超长被跳过
    try:
        # 2. 从文件末尾扫描，优先拿到最后一个完整 assistant 文本块。
        for line in _iter_lines_reverse(path):
            if not line:
                continue
            if len(line) > _MAX_LINE_BYTES:
                skipped_oversized = True
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if not isinstance(record, dict) or record.get("type") != "assistant":
                continue
            msg = record.get("message")
            if not isinstance(msg, dict):
                continue  # 尾部 last-prompt / custom-title 等元数据行无 message
            texts = _text_blocks(msg.get("content"))
            if texts:
                # 更新处有超长行被跳过 → 这段不是最终答复，可能是更早的中间过程文本，
                # 展示会误导（「最终答复」实为历史过程），宁可拒绝也不回退。
                if skipped_oversized:
                    return (None, ERR_REPLY_TOO_LONG)
                return (_cap("\n".join(texts)), None)
    except (OSError, PermissionError) as exc:
        logger.warning("claude transcript read failed (%s): %s", path, exc)
        return (None, ERR_READ_FAILED)
    return (None, ERR_NO_REPLY)


def read_codex_reply(session_id: str) -> tuple[Optional[str], Optional[str]]:
    """读取 Codex 会话的最终答复，优先 `final_answer` 阶段。

    Args:
        session_id: Codex 会话标识，必须满足安全白名单。

    Returns:
        二元组；成功时为（答复文本，None），失败时为（None，中文错误说明）。
    """
    # 1. 校验会话标识并定位其 rollout 文件，拒绝路径遍历。
    if not session_id:
        return (None, ERR_MISSING_ID)
    if not _safe_session_id(session_id):
        return (None, ERR_INVALID_ID)
    sessions = codex_home_dir() / "sessions"
    if not sessions.is_dir():
        return (None, ERR_NO_TRANSCRIPT)
    target_suffix = f"-{session_id}.jsonl"
    try:
        path = next(
            (p for p in sessions.rglob("rollout-*.jsonl") if p.name.endswith(target_suffix)),
            None,
        )
    except OSError:
        return (None, ERR_NO_TRANSCRIPT)
    if path is None:
        return (None, ERR_NO_TRANSCRIPT)
    fallback: Optional[str] = None
    skipped_oversized = False  # 逆序扫描中是否有更晚（更新）的行因超长被跳过
    try:
        # 2. 逆序读取 assistant 消息，优先返回明确标记的最终答复。
        for line in _iter_lines_reverse(path):
            if not line:
                continue
            if len(line) > _MAX_LINE_BYTES:
                skipped_oversized = True
                continue
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if not isinstance(record, dict) or record.get("type") != "response_item":
                continue
            payload = record.get("payload")
            if (
                not isinstance(payload, dict)
                or payload.get("type") != "message"
                or payload.get("role") != "assistant"
            ):
                continue
            texts = _text_blocks(payload.get("content"))
            if not texts:
                continue
            text = _cap("\n".join(texts))
            # 更新处有超长行被跳过 → 不把更早文本当最终答复展示
            if skipped_oversized:
                return (None, ERR_REPLY_TOO_LONG)
            if payload.get("phase") == "final_answer":
                return (text, None)
            if fallback is None:
                fallback = text
    except (OSError, PermissionError) as exc:
        logger.warning("codex rollout read failed (%s): %s", path, exc)
        return (None, ERR_READ_FAILED)
    if fallback is not None:
        return (fallback, None)
    return (None, ERR_NO_REPLY)


def _chatgpt_reply(events) -> dict:
    """ChatGPT 无本地记录：取事件流里最近一条非空 replyText（扩展随完成事件上报）。"""
    if not events:
        return {"content": None, "error": ERR_NO_REPLY}
    latest: Optional[str] = None
    for event in events:  # list_by_task 为 created_at ASC，顺序遍历末次非空即最新
        payload = event.get("payload")
        if isinstance(payload, dict):
            text = (payload.get("replyText") or "").strip()
            if text:
                latest = text
    if latest:
        return {"content": _cap(latest), "error": None}
    return {"content": None, "error": ERR_NO_REPLY}


def get_ai_reply(task: Task, recent_events: Optional[list] = None) -> dict:
    """按任务来源读取对应的最终 AI 答复。

    Args:
        task: 已持久化的任务，包含来源、会话标识和项目路径。
        recent_events: ChatGPT 任务的最近事件时间线；其他来源可传 None。

    Returns:
        包含 `content` 与 `error` 的字典；无法读取时 content 为 None。
    """
    session_id = task.external_task_id
    # 1. 本地会话型来源按各自文件协议读取，不跨来源猜测路径。
    if task.source == "CLAUDE_CODE":
        if not session_id:
            return {"content": None, "error": ERR_MISSING_ID}
        if not task.project_path:
            return {"content": None, "error": ERR_MISSING_PATH}
        text, err = read_claude_reply(task.project_path, session_id)
        return {"content": text, "error": err}
    if task.source == "CODEX":
        if not session_id:
            return {"content": None, "error": ERR_MISSING_ID}
        text, err = read_codex_reply(session_id)
        return {"content": text, "error": err}
    if task.source == "CHATGPT":
        # 2. ChatGPT 没有本地会话文件，只从浏览器扩展上报的事件中取答复。
        return _chatgpt_reply(recent_events)
    return {"content": None, "error": ERR_UNSUPPORTED}


# ---- 进程内 TTL 缓存（task_id → 结果） ----
_cache: dict[int, tuple[float, dict]] = {}
_cache_lock = threading.Lock()


def _cache_get(task_id: int) -> Optional[dict]:
    with _cache_lock:
        entry = _cache.get(task_id)
        if entry is None:
            return None
        expire_at, result = entry
        if time.monotonic() > expire_at:
            _cache.pop(task_id, None)
            return None
        return result


def _cache_put(task_id: int, result: dict) -> None:
    content = result.get("content")
    if not isinstance(content, str) or len(content) > _CACHE_MAX_CONTENT_CHARS:
        return
    with _cache_lock:
        now = time.monotonic()
        if len(_cache) >= _CACHE_MAX:
            expired = [k for k, (exp, _) in _cache.items() if now > exp]
            for k in expired:
                _cache.pop(k, None)
            if len(_cache) >= _CACHE_MAX:
                oldest = min(_cache, key=lambda k: _cache[k][0])
                _cache.pop(oldest, None)
        _cache[task_id] = (now + _CACHE_TTL_SEC, result)
