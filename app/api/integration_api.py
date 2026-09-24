"""接入集成 API：桌面端向导/体检读取三平台适配器状态，并执行一键接入。

- Claude Code：向 ~/.claude/settings.json 合并 UserPromptSubmit/Notification/Stop 钩子（幂等）
- Codex：保留原 notify 转发，并通过 ~/.codex/hooks.json 在提问时上报运行状态
- ChatGPT：接收 Chrome 扩展心跳（5min），判断扩展在线状态
"""

import functools
import json
import logging
import os
import shutil
import sys
import time
from collections.abc import Iterable, Mapping
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
_CODEX_PROMPT_HOOK_MARKER = "prompt_hook.py"
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
    """把 TOML notify 字符串或数组规范为命令参数列表。"""
    if isinstance(notify, str):
        return [notify]
    if not isinstance(notify, (str, bytes, dict)) and hasattr(notify, "__iter__"):
        return [str(x) for x in notify]
    return []


def _path_key(path: str | Path) -> str:
    """比较配置中的本地路径，兼容 Windows 大小写和分隔符差异。"""
    return os.path.normcase(os.path.normpath(os.fspath(path))).casefold()


def _codex_notify_installed(
    commands: list[str], chain: Path | None = None, python: str | None = None
) -> bool:
    """只把当前解释器和当前链脚本都匹配的 notify 视为已安装。"""
    chain = chain or _codex_chain()
    python = python or _adapter_python()
    return bool(
        chain
        and python
        and chain.is_file()
        and len(commands) == 2
        and _path_key(commands[0]) == _path_key(python)
        and _CODEX_CHAIN_MARKER in commands[1]
        and _path_key(commands[1]) == _path_key(chain)
    )


def _codex_installed() -> bool:
    if not CODEX_CONFIG.exists():
        return False
    try:
        doc = tomlkit.parse(CODEX_CONFIG.read_text(encoding="utf-8"))
        return _codex_notify_installed(_as_command_list(doc.get("notify")))
    except Exception:
        return False


def _codex_hooks_config_path() -> Path:
    return CODEX_CONFIG.with_name("hooks.json")


def _has_codex_prompt_hook(config: dict[str, Any], expected_command: str | None = None) -> bool:
    """检查 UserPromptSubmit 中是否登记当前 AI Task Hub 命令。"""
    hooks = config.get("hooks")
    entries = hooks.get("UserPromptSubmit") if isinstance(hooks, dict) else None
    if not isinstance(entries, list):
        return False
    for entry in entries:
        handlers = entry.get("hooks") if isinstance(entry, dict) else None
        if not isinstance(handlers, list):
            continue
        for handler in handlers:
            if not isinstance(handler, dict):
                continue
            commands = [str(handler.get(field) or "") for field in ("command", "commandWindows", "command_windows")]
            if any(_CODEX_PROMPT_HOOK_MARKER in command for command in commands) and (
                expected_command is None or expected_command in commands
            ):
                return True
    return False


def _toml_value(table: Any, key: str) -> Any:
    """兼容读取 tomlkit 表和普通映射中的字段。"""
    return table.get(key) if hasattr(table, "get") else None


def _codex_prompt_hook_policy_error(config_doc: Any) -> str | None:
    """读取本地可见的用户设置和组织策略，避免把被禁用的 hook 报成已接入。"""
    features = _toml_value(config_doc, "features")
    user_hooks = _toml_value(features, "hooks")
    if user_hooks is None:
        user_hooks = _toml_value(features, "codex_hooks")
    program_data = os.environ.get("PROGRAMDATA")
    if not program_data and os.name == "nt":
        program_data = r"C:\ProgramData"

    managed_hooks: bool | None = None
    managed_only = False
    if program_data:
        requirements_path = Path(program_data) / "OpenAI" / "Codex" / "requirements.toml"
        if requirements_path.exists():
            try:
                requirements = tomlkit.parse(requirements_path.read_text(encoding="utf-8"))
            except Exception:
                return "无法读取 Codex 组织策略，不能确认提问 hook 是否允许"
            managed_features = _toml_value(requirements, "features")
            managed_hooks = _toml_value(managed_features, "hooks")
            if managed_hooks is None:
                managed_hooks = _toml_value(managed_features, "codex_hooks")
            managed_only = _toml_value(requirements, "allow_managed_hooks_only") is True

    if managed_hooks is False:
        return "Codex 组织策略已禁用 hooks，提问状态 hook 不会运行"
    if managed_only:
        return "Codex 组织策略仅允许托管 hooks，AI Task Hub 的用户 hook 不会运行"
    if user_hooks is False and managed_hooks is not True:
        return "Codex config.toml 中 features.hooks/codex_hooks=false，提问状态 hook 不会运行"
    return None


def _codex_prompt_hook_status() -> tuple[bool, str | None]:
    """返回本地 hook 配置状态及已知的路径或策略问题。"""
    path = _codex_hooks_config_path()
    config_doc: Any = tomlkit.document()
    if CODEX_CONFIG.exists():
        try:
            config_doc = tomlkit.parse(CODEX_CONFIG.read_text(encoding="utf-8"))
        except Exception:
            return False, "无法解析 Codex config.toml，不能确认提问 hook 是否允许"
    policy_error = _codex_prompt_hook_policy_error(config_doc)
    if policy_error:
        return False, policy_error
    if not path.exists():
        return False, None
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False, "Codex hooks.json 无法读取或解析"
    chain = _codex_chain()
    python = _adapter_python()
    prompt_hook = chain.with_name(_CODEX_PROMPT_HOOK_MARKER) if chain else None
    if not isinstance(config, dict) or chain is None or prompt_hook is None or not prompt_hook.is_file() or python is None:
        return False, None
    expected_command = f'"{python}" "{prompt_hook}"'
    if _has_codex_prompt_hook(config, expected_command):
        return True, None
    if _has_codex_prompt_hook(config):
        return False, "hooks.json 中仍指向旧版提问 hook 路径，请重新接入以更新路径"
    return False, None


def _codex_prompt_hook_installed() -> bool:
    """兼容状态调用方，只返回当前 hook 是否有效登记。"""
    return _codex_prompt_hook_status()[0]


def _ensure_codex_prompt_hook(entries: list[Any], command: str) -> bool:
    """更新已有 AI Task Hub hook 的路径，保留其他 hook 配置。"""
    changed = False
    matched = False
    for entry in entries:
        handlers = entry.get("hooks") if isinstance(entry, dict) else None
        if not isinstance(handlers, list):
            continue
        for handler in handlers:
            if not isinstance(handler, dict):
                continue
            fields = ("command", "commandWindows", "command_windows")
            if not any(_CODEX_PROMPT_HOOK_MARKER in str(handler.get(field) or "") for field in fields):
                continue
            matched = True
            if handler.get("type") != "command":
                handler["type"] = "command"
                changed = True
            for field in fields[:2]:
                if handler.get(field) != command:
                    handler[field] = command
                    changed = True
            if "command_windows" in handler and handler["command_windows"] != command:
                handler["command_windows"] = command
                changed = True
    if matched:
        return changed

    entry = {"hooks": [{"type": "command", "command": command, "commandWindows": command, "timeout": 3}]}
    entries.append(entry)
    return True


def _write_bytes_atomic(path: Path, content: bytes) -> None:
    """先写临时文件再替换配置，避免留下截断文件。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = path.with_name(path.name + ".aihub.tmp")
    try:
        temp_path.write_bytes(content)
        temp_path.replace(path)
    finally:
        try:
            temp_path.unlink(missing_ok=True)
        except OSError:
            pass


@router.post("/codex/install")
@_serialized
def install_codex() -> dict[str, Any]:
    """安装完成通知与提问状态 hook，并保留已有 notify 命令的转发。

    Returns:
        包含安装结果的对象；失败时附带 error，成功时说明是否改动和是否接管原 notify。
    """
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
    prompt_hook = chain.with_name(_CODEX_PROMPT_HOOK_MARKER)
    if not prompt_hook.is_file():
        return {"success": False, "changed": False, "error": "未找到 Codex 提问状态 hook（应用资源缺失），请重新安装应用"}
    forward_target = _codex_forward_target()

    doc: Any = tomlkit.document()
    if CODEX_CONFIG.exists():
        try:
            doc = tomlkit.parse(CODEX_CONFIG.read_text(encoding="utf-8"))
        except Exception:
            return {"success": False, "changed": False, "error": "config.toml 解析失败，请手工检查"}

    notify_value = doc.get("notify")
    if notify_value is not None and (
        isinstance(notify_value, Mapping)
        or not isinstance(notify_value, (str, Iterable))
        or (not isinstance(notify_value, str) and any(not isinstance(item, str) for item in notify_value))
    ):
        return {"success": False, "changed": False, "error": "config.toml 的 notify 必须是字符串或字符串数组，未修改配置"}
    existing = _as_command_list(notify_value)
    notify_installed = _codex_notify_installed(existing, chain, python)
    policy_error = _codex_prompt_hook_policy_error(doc)
    if policy_error:
        return {"success": False, "changed": False, "error": policy_error}

    hooks_config_path = _codex_hooks_config_path()
    hooks_config: dict[str, Any] = {}
    if hooks_config_path.exists():
        try:
            loaded = json.loads(hooks_config_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {"success": False, "changed": False, "error": "hooks.json 解析失败，请手工检查"}
        if not isinstance(loaded, dict):
            return {"success": False, "changed": False, "error": "hooks.json 顶层必须是对象，未修改配置"}
        hooks_config = loaded

    prompt_command = f'"{python}" "{prompt_hook}"'
    if "hooks" not in hooks_config:
        hooks = {}
        hooks_config["hooks"] = hooks
    else:
        hooks = hooks_config["hooks"]
    if not isinstance(hooks, dict):
        return {"success": False, "changed": False, "error": "hooks.json 的 hooks 字段必须是对象，未修改配置"}
    if "UserPromptSubmit" not in hooks:
        entries = []
        hooks["UserPromptSubmit"] = entries
    else:
        entries = hooks["UserPromptSubmit"]
    if not isinstance(entries, list):
        return {"success": False, "changed": False, "error": "hooks.json 的 UserPromptSubmit 字段必须是数组，未修改配置"}

    if not notify_installed:
        doc["notify"] = [python, str(chain)]

    hooks_changed = _ensure_codex_prompt_hook(entries, prompt_command)
    if notify_installed and not hooks_changed:
        return {"success": True, "changed": False}

    try:
        config_existed = CODEX_CONFIG.exists()
        original_config = CODEX_CONFIG.read_bytes() if config_existed else None
        forward_existed = forward_target.exists()
        original_forward = forward_target.read_bytes() if forward_existed else None
    except OSError as exc:
        return {"success": False, "changed": False, "error": f"无法备份 Codex 配置：{exc}"}
    forward_changed = False

    # 升级旧适配器时迁移它旁边保存的原 notify，保证切换脚本路径后仍能转发。
    old_chain = next((Path(command) for command in existing if _CODEX_CHAIN_MARKER in command), None)
    if old_chain is not None and _path_key(old_chain) != _path_key(chain) and not forward_existed:
        old_forward = old_chain.parent / "forward_target.json"
        if old_forward.is_file():
            try:
                shutil.copy2(old_forward, forward_target)
                forward_changed = True
            except OSError as exc:
                try:
                    forward_target.unlink(missing_ok=True)
                except OSError:
                    pass
                return {"success": False, "changed": False, "error": f"迁移旧 notify 转发配置失败：{exc}"}
    elif existing and old_chain is None and not forward_existed:
        try:
            _write_bytes_atomic(
                forward_target,
                (json.dumps({"command": existing}, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
            )
            forward_changed = True
            logger.info("原 Codex notify 命令已保存至 %s", forward_target)
        except OSError as exc:
            return {"success": False, "changed": False, "error": f"保存原 notify 命令失败：{exc}"}

    config_written = False
    try:
        if not notify_installed:
            _write_bytes_atomic(CODEX_CONFIG, tomlkit.dumps(doc).encode("utf-8"))
            config_written = True
            logger.info("Codex notify 链式配置已写入 %s", CODEX_CONFIG)
        if hooks_changed:
            _write_bytes_atomic(
                hooks_config_path,
                (json.dumps(hooks_config, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
            )
    except Exception as exc:
        rollback_errors: list[str] = []
        if config_written:
            try:
                if original_config is None:
                    CODEX_CONFIG.unlink(missing_ok=True)
                else:
                    _write_bytes_atomic(CODEX_CONFIG, original_config)
            except OSError as rollback_exc:
                rollback_errors.append(f"恢复 config.toml 失败：{rollback_exc}")
        if forward_changed:
            try:
                if original_forward is None:
                    forward_target.unlink(missing_ok=True)
                else:
                    _write_bytes_atomic(forward_target, original_forward)
            except OSError as rollback_exc:
                rollback_errors.append(f"恢复 notify 转发文件失败：{rollback_exc}")
        detail = f"配置写入失败：{exc}"
        if rollback_errors:
            detail += "；" + "；".join(rollback_errors)
        return {"success": False, "changed": False, "error": detail}

    return {
        "success": True,
        "changed": not notify_installed or hooks_changed or forward_changed,
        "forwardTarget": forward_target.exists(),
    }


@router.get("/codex/stale-check")
def codex_stale_check() -> dict[str, Any]:
    """检测早于 Codex notify 或 hook 配置启动的进程。

    Returns:
        包含接入标志、配置路径、运行进程和过期进程列表的状态对象。
    """
    installed = _codex_installed()
    config_paths = (CODEX_CONFIG, _codex_hooks_config_path())
    config_mtimes = [path.stat().st_mtime for path in config_paths if path.exists()]
    config_mtime = max(config_mtimes) if config_mtimes else None
    processes = _codex_processes()
    stale = [
        p for p in processes
        if config_mtime is not None and p.get("createTime") and p["createTime"] < config_mtime
    ]
    prompt_hook_installed, prompt_hook_error = _codex_prompt_hook_status()
    return {
        "installed": installed,
        "promptHookInstalled": prompt_hook_installed,
        "promptHookError": prompt_hook_error,
        "hooksConfigPath": str(_codex_hooks_config_path()),
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
