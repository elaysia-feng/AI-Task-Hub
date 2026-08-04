"""接入集成 API：桌面端向导/体检读取三平台适配器状态，并执行一键接入。

- Claude Code：向 ~/.claude/settings.json 合并 UserPromptSubmit/Notification/Stop 钩子（幂等）
- Codex：改写 ~/.codex/config.toml 的 notify 为链式转发（原命令存入 forward_target.json）
- ChatGPT：接收 Chrome 扩展心跳（5min），判断扩展在线状态
"""

import json
import logging
import time
from pathlib import Path
from threading import Lock
from typing import Any

import tomlkit
from fastapi import APIRouter
from pydantic import BaseModel

from app.logging_config import log_dir
from shared.constants import APP_VERSION

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/api/integrations", tags=["integrations"])

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent

CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"
CLAUDE_ADAPTER = _REPO_ROOT / "adapters" / "claude-code" / "claude_adapter.py"

CODEX_CONFIG = Path.home() / ".codex" / "config.toml"
CODEX_CHAIN = _REPO_ROOT / "adapters" / "codex" / "notify_chain.py"
CODEX_FORWARD_TARGET = _REPO_ROOT / "adapters" / "codex" / "forward_target.json"

CHATGPT_EXT_DIR = _REPO_ROOT / "adapters" / "chatgpt-extension"
HEARTBEAT_FILE = log_dir() / "chatgpt_heartbeat.json"
HEARTBEAT_TTL_SEC = 10 * 60

_CLAUDE_HOOK_MARKER = "claude_adapter.py"
_CODEX_CHAIN_MARKER = "notify_chain.py"
_CODEX_PROCESS_CACHE_TTL_SEC = 15.0
_codex_process_cache_at = 0.0
_codex_process_cache: list[dict[str, Any]] = []
_codex_process_lock = Lock()


def _venv_python() -> str:
    """后端虚拟环境 Python 的绝对路径（会被拼进 settings.json / config.toml 命令）。

    A31：路径来自仓库固定位置，但作为命令拼接进配置文件，必须校验为绝对路径
    且不含引号/换行，防止破坏命令拼接或注入；缺失时告警而不是静默写坏配置。
    """
    exe = _REPO_ROOT / ".venv" / "Scripts" / "python.exe"
    text = str(exe)
    if not exe.is_absolute():
        raise ValueError(f"_venv_python: 需要绝对路径，收到 {text!r}")
    if any(c in text for c in ('"', "\n", "\r")):
        raise ValueError(f"_venv_python: 路径含引号/换行，拒绝写入配置: {text!r}")
    if not exe.is_file():
        logger.warning("虚拟环境 Python 不存在（%s），写入的钩子/notify 命令将不可用", exe)
    return text


def _scan_codex_processes(psutil: Any) -> list[dict[str, Any]]:
    """只读取 Codex.exe 和 node.exe 候选进程，避免遍历时查询所有进程命令行。"""
    found: list[dict[str, Any]] = []
    for proc in psutil.process_iter(["name"]):
        try:
            name = (proc.info.get("name") or "").lower()
            is_codex_exe = "codex" in name
            if not is_codex_exe and name not in {"node", "node.exe"}:
                continue
            cmdline = [] if is_codex_exe else proc.cmdline()
            if not is_codex_exe and "codex" not in " ".join(cmdline).lower():
                continue
            found.append({
                "pid": proc.pid,
                "name": proc.info.get("name"),
                "createTime": proc.create_time(),
            })
        except (
            psutil.NoSuchProcess,
            psutil.AccessDenied,
            getattr(psutil, "ZombieProcess", psutil.NoSuchProcess),
        ):
            continue
    return found


def _codex_processes() -> list[dict[str, Any]]:
    """本机 Codex 相关进程；短时缓存避免设置页刷新时重复扫描 Windows 进程。"""
    global _codex_process_cache_at, _codex_process_cache
    try:
        import psutil
    except ImportError:
        return []

    now = time.monotonic()
    if now - _codex_process_cache_at < _CODEX_PROCESS_CACHE_TTL_SEC:
        return list(_codex_process_cache)
    with _codex_process_lock:
        now = time.monotonic()
        if now - _codex_process_cache_at < _CODEX_PROCESS_CACHE_TTL_SEC:
            return list(_codex_process_cache)
        _codex_process_cache = _scan_codex_processes(psutil)
        _codex_process_cache_at = now
        return list(_codex_process_cache)


# ---------- Claude Code ----------

# 适配器 HOOK_EVENT_MAP 支持的三类钩子（勿再注册 PostToolUse：无映射，注册了也是空转）
_CLAUDE_HOOK_EVENTS = ("UserPromptSubmit", "Notification", "Stop")


def _claude_hook_command() -> str:
    return f'"{_venv_python()}" "{CLAUDE_ADAPTER}"'


def _claude_installed() -> bool:
    if not CLAUDE_SETTINGS.exists():
        return False
    try:
        text = CLAUDE_SETTINGS.read_text(encoding="utf-8")
        return _CLAUDE_HOOK_MARKER in text
    except OSError:
        return False


@router.post("/claude-code/install")
def install_claude_code() -> dict[str, Any]:
    """向 settings.json 的 hooks 追加三类钩子事件，其余配置原样保留。"""
    data: dict[str, Any] = {}
    if CLAUDE_SETTINGS.exists():
        try:
            data = json.loads(CLAUDE_SETTINGS.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {"success": False, "changed": False, "error": "settings.json 解析失败，请手工检查"}
    if _CLAUDE_HOOK_MARKER in json.dumps(data, ensure_ascii=False):
        return {"success": True, "changed": False}

    hooks = data.setdefault("hooks", {})
    for event_name in _CLAUDE_HOOK_EVENTS:
        entries = hooks.setdefault(event_name, [])
        entries.append({"hooks": [{"type": "command", "command": _claude_hook_command()}]})

    CLAUDE_SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    CLAUDE_SETTINGS.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    logger.info("Claude Code hook 已写入 %s", CLAUDE_SETTINGS)
    return {"success": True, "changed": True}


# ---------- Codex ----------

def _as_command_list(notify: Any) -> list[str]:
    if isinstance(notify, str):
        return [notify]
    if isinstance(notify, list):
        return [str(x) for x in notify]
    return []


def _codex_installed() -> bool:
    if not CODEX_CONFIG.exists():
        return False
    try:
        return _CODEX_CHAIN_MARKER in CODEX_CONFIG.read_text(encoding="utf-8")
    except OSError:
        return False


@router.post("/codex/install")
def install_codex() -> dict[str, Any]:
    """config.toml 的 notify 改写为链式适配器；原 notify 命令存入 forward_target.json 继续转发。"""
    doc: Any = tomlkit.document()
    if CODEX_CONFIG.exists():
        try:
            doc = tomlkit.parse(CODEX_CONFIG.read_text(encoding="utf-8"))
        except Exception:
            return {"success": False, "changed": False, "error": "config.toml 解析失败，请手工检查"}

    existing = _as_command_list(doc.get("notify"))
    if any(_CODEX_CHAIN_MARKER in c for c in existing):
        return {"success": True, "changed": False}

    if existing and not CODEX_FORWARD_TARGET.exists():
        CODEX_FORWARD_TARGET.write_text(
            json.dumps({"command": existing}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        logger.info("原 Codex notify 命令已保存至 %s", CODEX_FORWARD_TARGET)

    doc["notify"] = [_venv_python(), str(CODEX_CHAIN)]
    CODEX_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CODEX_CONFIG.write_text(tomlkit.dumps(doc), encoding="utf-8")
    logger.info("Codex notify 链式配置已写入 %s", CODEX_CONFIG)
    return {"success": True, "changed": True, "forwardTarget": bool(existing)}


@router.get("/codex/stale-check")
def codex_stale_check() -> dict[str, Any]:
    """检测早于 config.toml 修改时间启动的 Codex 进程（它们未加载新 notify 配置）。"""
    installed = _codex_installed()
    config_mtime = CODEX_CONFIG.stat().st_mtime if CODEX_CONFIG.exists() else None
    processes = _codex_processes()
    stale = [
        p for p in processes
        if config_mtime is not None and p.get("createTime") and p["createTime"] < config_mtime
    ]
    return {
        "installed": installed,
        "exeRunning": bool(processes),
        "processCount": len(processes),
        "staleProcesses": stale,
        "stale": bool(stale),
    }


# ---------- ChatGPT 扩展心跳 ----------

class Heartbeat(BaseModel):
    version: str = ""


@router.post("/chatgpt/heartbeat")
def chatgpt_heartbeat(hb: Heartbeat) -> dict[str, Any]:
    payload = {"ts": time.time(), "version": hb.version}
    try:
        HEARTBEAT_FILE.write_text(json.dumps(payload), encoding="utf-8")
    except OSError:
        logger.warning("心跳落盘失败: %s", HEARTBEAT_FILE)
    return {"success": True}


def _chatgpt_online() -> dict[str, Any]:
    if not HEARTBEAT_FILE.exists():
        return {"installed": False, "lastHeartbeat": None, "version": None}
    try:
        data = json.loads(HEARTBEAT_FILE.read_text(encoding="utf-8"))
        ts = float(data.get("ts", 0))
    except (OSError, ValueError):
        return {"installed": False, "lastHeartbeat": None, "version": None}
    fresh = (time.time() - ts) < HEARTBEAT_TTL_SEC
    return {
        "installed": fresh,
        "lastHeartbeat": ts,
        "version": data.get("version"),
        "stale": not fresh,
    }


# ---------- 汇总 ----------

@router.get("/status")
def integrations_status() -> dict[str, Any]:
    codex = codex_stale_check()
    return {
        "claudeCode": {
            "installed": _claude_installed(),
            "settingsPath": str(CLAUDE_SETTINGS),
        },
        "codex": {
            "configPath": str(CODEX_CONFIG),
            "forwardTarget": CODEX_FORWARD_TARGET.exists(),
            **codex,
        },
        "chatgpt": {
            **_chatgpt_online(),
            "extensionDir": str(CHATGPT_EXT_DIR),
        },
        "backend": {"version": APP_VERSION, "python": _venv_python()},
    }
