"""接入集成 API：桌面端向导/体检读取三平台适配器状态，并执行一键接入。

- Claude Code：向 ~/.claude/settings.json 合并 UserPromptSubmit/Notification/Stop 钩子（幂等）
- Codex：改写 ~/.codex/config.toml 的 notify 为链式转发（原命令存入 forward_target.json）
- ChatGPT：接收 Chrome 扩展心跳（5min），判断扩展在线状态
"""

import functools
import json
import logging
import os
import shutil
import sys
import time
from pathlib import Path
from threading import Lock
from typing import Any

import tomlkit
from fastapi import APIRouter
from pydantic import BaseModel

from app.logging_config import log_dir, user_data_dir
from shared.constants import APP_VERSION

logger = logging.getLogger(__name__)
router = APIRouter(prefix="/api/integrations", tags=["integrations"])

# 一键接入端点做读-改-写（settings.json / config.toml）。FastAPI 同步端点跑在线程池，
# 不加锁时两个并发请求可交错覆盖彼此的修改（lost update）。单进程部署下进程内锁足够。
_INSTALL_LOCK = Lock()


def _serialized(fn):
    """给 FastAPI 同步端点加进程内锁，串行化读-改-写区间。"""

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        with _INSTALL_LOCK:
            return fn(*args, **kwargs)

    return wrapper

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent

CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"

CODEX_CONFIG = Path.home() / ".codex" / "config.toml"

HEARTBEAT_FILE = log_dir() / "chatgpt_heartbeat.json"
HEARTBEAT_TTL_SEC = 10 * 60

# PyInstaller 打包态资源在 _MEIPASS 内的相对路径（开发态忽略，直接走 _REPO_ROOT）
_CHATGPT_EXT_BUNDLED = "adapters/chatgpt-extension"


def _bundled_path(relative: str) -> Path:
    """打包态资源路径：PyInstaller 解压目录优先，否则仓库根（开发态）。"""
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        return Path(meipass) / relative
    return _REPO_ROOT / relative


def _materialize_bundled(relative: str, target: Path, exclude: frozenset[str]) -> Path:
    """把打包进 exe 的目录物化到持久用户目录（幂等：目标缺失或源更新时覆盖）。

    ChatGPT 扩展被 Chrome「加载已解压的扩展」引用、Claude/Codex 适配器被外部 Python
    执行，都必须指向稳定可写的路径（_MEIPASS 每次运行重建）。文件监听对覆盖热加载，
    升级后无需用户重装；任何失败仅告警，不阻断调用方。
    """
    source = _bundled_path(relative)
    if not source.is_dir():
        logger.warning("[integrations] 打包内缺少资源 %s: %s", relative, source)
        return target
    try:
        target.mkdir(parents=True, exist_ok=True)
        for src_file in source.iterdir():
            if src_file.name in exclude or not src_file.is_file():
                continue
            dst_file = target / src_file.name
            try:
                if dst_file.exists() and src_file.stat().st_mtime <= dst_file.stat().st_mtime:
                    continue
                shutil.copy2(src_file, dst_file)
            except OSError:
                continue
    except OSError as exc:
        logger.warning("[integrations] 资源物化失败 %s: %s", relative, exc)
    return target


def _chatgpt_extension_dir() -> Path:
    """ChatGPT 扩展目录：开发态指向仓库源码（改动即时生效）；
    打包态指向持久用户目录 %APPDATA%/AI Task Hub/chatgpt-extension，首次运行从包内复制。
    """
    if not getattr(sys, "frozen", False):
        return _REPO_ROOT / "adapters" / "chatgpt-extension"
    return _materialize_bundled(
        _CHATGPT_EXT_BUNDLED,
        user_data_dir() / "chatgpt-extension",
        frozenset({"__pycache__"}),
    )


def _claude_adapter() -> Path | None:
    """Claude Code 适配器脚本路径（claude_adapter.py 与其 session_titles.json 须同目录）。

    打包态物化到用户目录，供 ~/.claude/settings.json 的钩子命令引用。
    脚本文件不存在时返回 None（物化源缺失/被打包遗漏），调用方在写入配置前报错，
    避免把不存在的命令写进 settings.json 造成静默失败（review LOW fail-open）。
    """
    if not getattr(sys, "frozen", False):
        candidate = _REPO_ROOT / "adapters" / "claude-code" / "claude_adapter.py"
    else:
        candidate = _materialize_bundled(
            "adapters/claude-code",
            user_data_dir() / "adapters" / "claude-code",
            frozenset({"__pycache__"}),
        ) / "claude_adapter.py"
    if not candidate.is_file():
        logger.warning("[integrations] Claude Code 适配器脚本缺失: %s", candidate)
        return None
    return candidate


def _codex_chain() -> Path | None:
    """Codex 链式 notify 适配器路径。打包态物化到用户目录（forward_target.json 由
    install 落盘到同一目录，notify_chain 从自身目录读取，目录必须可写）。
    脚本不存在时返回 None，install 写入配置前报错（review LOW fail-open）。"""
    if not getattr(sys, "frozen", False):
        candidate = _REPO_ROOT / "adapters" / "codex" / "notify_chain.py"
    else:
        candidate = _materialize_bundled(
            "adapters/codex",
            user_data_dir() / "adapters" / "codex",
            frozenset({"__pycache__", "forward_target.json", "notify_debug.log"}),
        ) / "notify_chain.py"
    if not candidate.is_file():
        logger.warning("[integrations] Codex 适配器脚本缺失: %s", candidate)
        return None
    return candidate


def _codex_forward_target() -> Path:
    """原 Codex notify 命令存档路径（install 时写入，notify_chain 从自身目录读取）。"""
    if not getattr(sys, "frozen", False):
        return _REPO_ROOT / "adapters" / "codex" / "forward_target.json"
    return user_data_dir() / "adapters" / "codex" / "forward_target.json"


_CLAUDE_HOOK_MARKER = "claude_adapter.py"
_CODEX_CHAIN_MARKER = "notify_chain.py"
_CODEX_PROCESS_CACHE_TTL_SEC = 15.0
_codex_process_cache_at = 0.0
_codex_process_cache: list[dict[str, Any]] = []
_codex_process_lock = Lock()


def _adapter_python() -> str | None:
    """运行适配器脚本的 Python 解释器命令（会被拼进 settings.json / config.toml）。

    开发态：仓库 .venv 的 python.exe（与后端同依赖）。
    打包态：AIHUB_PYTHON 环境变量优先，否则 PATH 上的 python / python3 / py
    （Windows 安装 Python 时通常可用）；都找不到返回 None，调用方在写入配置前报错，
    不写坏命令。返回前校验不含引号/换行（A31），防破坏命令拼接或注入。
    """
    if not getattr(sys, "frozen", False):
        # 开发态：路径由仓库布局决定（Windows .venv/Scripts/python.exe，POSIX .venv/bin/python）。
        # 确定性返回、不做存在性校验：venv 缺失属开发环境问题，由运行期自然暴露；
        # 若在此校验会让无 .venv 的 CI 测试环境解析为 None（存在性校验只属于打包态）。
        scripts = "Scripts" if sys.platform == "win32" else "bin"
        exe = "python.exe" if sys.platform == "win32" else "python"
        return str(_REPO_ROOT / ".venv" / scripts / exe)

    # 打包态：AIHUB_PYTHON 优先，否则 PATH 上的 python/python3/py；解析后确认解释器
    # 真实存在，找不到则返回 None：避免把不存在的路径写进钩子/notify 造成静默失败
    # （review MEDIUM fail-open）。相对名经 PATH 解析为绝对路径；绝对路径须真实存在。
    cmd = os.environ.get("AIHUB_PYTHON", "").strip()
    if not cmd:
        cmd = shutil.which("python") or shutil.which("python3") or shutil.which("py") or ""
    if not cmd:
        return None
    if any(c in cmd for c in ('"', "\n", "\r")):
        logger.warning("Python 命令含引号/换行，拒绝写入配置: %s", cmd)
        return None
    if not os.path.isabs(cmd):
        resolved = shutil.which(cmd)
        if resolved is None:
            logger.warning("Python 解释器未找到: %s", cmd)
            return None
        cmd = resolved
    elif not os.path.isfile(cmd):
        logger.warning("Python 解释器不存在（%s），拒绝写入配置", cmd)
        return None
    return cmd


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


def _claude_installed() -> bool:
    if not CLAUDE_SETTINGS.exists():
        return False
    try:
        text = CLAUDE_SETTINGS.read_text(encoding="utf-8")
        return _CLAUDE_HOOK_MARKER in text
    except OSError:
        return False


@router.post("/claude-code/install")
@_serialized
def install_claude_code() -> dict[str, Any]:
    """向 settings.json 的 hooks 追加三类钩子事件，其余配置原样保留。"""
    python = _adapter_python()
    if python is None:
        return {
            "success": False,
            "changed": False,
            "error": "未检测到本机 Python，无法接入 Claude Code（打包版需安装 Python，或设置 AIHUB_PYTHON 后重启）",
        }
    adapter = _claude_adapter()
    if adapter is None:
        return {"success": False, "changed": False, "error": "未找到 Claude Code 适配器脚本（应用资源缺失），请重新安装应用"}
    command = f'"{python}" "{adapter}"'

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
        entries.append({"hooks": [{"type": "command", "command": command}]})

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
@_serialized
def install_codex() -> dict[str, Any]:
    """config.toml 的 notify 改写为链式适配器；原 notify 命令存入 forward_target.json 继续转发。"""
    python = _adapter_python()
    if python is None:
        return {
            "success": False,
            "changed": False,
            "error": "未检测到本机 Python，无法接入 Codex（打包版需安装 Python，或设置 AIHUB_PYTHON 后重启）",
        }
    chain = _codex_chain()
    if chain is None:
        return {"success": False, "changed": False, "error": "未找到 Codex 适配器脚本（应用资源缺失），请重新安装应用"}
    forward_target = _codex_forward_target()

    doc: Any = tomlkit.document()
    if CODEX_CONFIG.exists():
        try:
            doc = tomlkit.parse(CODEX_CONFIG.read_text(encoding="utf-8"))
        except Exception:
            return {"success": False, "changed": False, "error": "config.toml 解析失败，请手工检查"}

    existing = _as_command_list(doc.get("notify"))
    if any(_CODEX_CHAIN_MARKER in c for c in existing):
        return {"success": True, "changed": False}

    if existing and not forward_target.exists():
        forward_target.parent.mkdir(parents=True, exist_ok=True)
        forward_target.write_text(
            json.dumps({"command": existing}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        logger.info("原 Codex notify 命令已保存至 %s", forward_target)

    doc["notify"] = [python, str(chain)]
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
            "forwardTarget": _codex_forward_target().exists(),
            **codex,
        },
        "chatgpt": {
            **_chatgpt_online(),
            "extensionDir": str(_chatgpt_extension_dir()),
        },
        "backend": {"version": APP_VERSION, "python": _adapter_python() or ""},
    }
