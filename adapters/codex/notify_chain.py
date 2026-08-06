"""Codex notify 链式适配器：上报 AI Task Hub 的同时转发给原始 notify 目标。

Codex 只支持单个 notify 命令，而本机已被 Codex 桌面端占用。
config.toml 中的 notify 指向本脚本后，事件流向：
    codex → notify_chain.py <payload>
              ├── POST /api/events（AI Task Hub）
              └── 原命令（forward_target.json，Codex 桌面通知）

任何异常静默退出 0，绝不阻塞 Codex。
"""

import json
import logging
import os
import re
import subprocess
import sys
import time
import urllib.request
from datetime import datetime
from logging.handlers import RotatingFileHandler

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import codex_notify_to_event  # noqa: E402

# 端口允许用 AIHUB_PORT 覆盖（冒烟测试并行实例、端口冲突场景）
API_URL = f"http://127.0.0.1:{int(os.environ.get('AIHUB_PORT', '17891'))}/api/events"
TIMEOUT_SEC = 2
FORWARD_TARGET_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "forward_target.json")
DEBUG_LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notify_debug.log")


# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


_debug_logger = None


def _get_debug_logger():
    global _debug_logger
    if _debug_logger is None:
        try:
            _debug_logger = logging.getLogger("notify_debug")
            _debug_logger.setLevel(logging.INFO)
            _debug_logger.addHandler(
                RotatingFileHandler(
                    DEBUG_LOG_PATH, maxBytes=1_000_000, backupCount=3, encoding="utf-8"
                )
            )
            _debug_logger.handlers[0].setFormatter(
                logging.Formatter("%(message)s")
            )
        except (OSError, PermissionError) as exc:
            logging.warning("[_get_debug_logger] failed to initialize debug logger: %s", exc)
            _debug_logger = None
    return _debug_logger


def debug_log(entry: dict) -> None:
    """排障日志：记录每次 notify 触发的载荷与处理结果，任何失败静默。"""
    try:
        entry.setdefault("ts", datetime.now().isoformat(timespec="seconds"))
        logger = _get_debug_logger()
        if logger:
            logger.info(json.dumps(entry, ensure_ascii=False))
        else:
            with open(DEBUG_LOG_PATH, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception:
        pass


def post_event(event: dict) -> str:
    try:
        body = json.dumps(event).encode("utf-8")
        request = urllib.request.Request(
            API_URL,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with _opener.open(request, timeout=TIMEOUT_SEC):
            pass
        return "ok"
    except Exception as exc:
        return f"error: {exc}"


# Codex 桌面 runtime 路径模式：.../runtimes/<runtime>/<hash>/<suffix>（hash 升级后变化）
_CODEX_RUNTIME_RE = re.compile(
    r"^(?P<root>.+[\\/]runtimes[\\/][^\\/]+[\\/])[^\\/]+(?P<suffix>[\\/].*)$",
    re.IGNORECASE,
)


def _resolve_stale_target(orig: str) -> str | None:
    """Codex 升级后 runtime hash 变化：按相同相对后缀在 runtimes 目录下找可执行文件。

    只处理已知的 Codex runtimes 路径模式，其余命令原样走「目标不存在」分支。
    多个候选时选 mtime 最新的（hash 不透明，不能用字典序比较）。
    """
    m = _CODEX_RUNTIME_RE.match(orig)
    if not m:
        return None
    root, suffix = m.group("root"), m.group("suffix").lstrip("\\/")
    candidates: list[tuple[float, str]] = []
    try:
        for entry in os.listdir(root):
            p = os.path.join(root, entry, suffix)
            if os.path.isfile(p):
                try:
                    candidates.append((os.path.getmtime(p), p))
                except OSError:
                    continue
    except OSError:
        return None
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][1]


def forward(payload_json: str) -> None:
    """把原始载荷转发给被接管的官方 notify 命令。"""
    try:
        with open(FORWARD_TARGET_PATH, encoding="utf-8") as f:
            target = json.load(f).get("command")
        if not target:
            # 配置缺失路径不再静默：记入诊断日志，便于排查「转发消失」问题（M10）
            debug_log({"stage": "forward_skipped", "reason": "forward_target.json 无 command 字段"})
            return
        resolved = os.path.abspath(target[0])
        if not os.path.exists(resolved):
            # 目标命令路径失效（如 Codex 升级后 runtime hash 变化）：先尝试自愈解析（M11）
            remapped = _resolve_stale_target(resolved)
            if remapped:
                debug_log({"stage": "forward_remapped", "from": target[0], "to": remapped})
                resolved = remapped
            else:
                debug_log({"stage": "forward_skipped", "reason": f"目标命令不存在且无法自愈: {resolved}"})
                return
        # pass payload as final argument, shell=False for cross-platform safety
        subprocess.run(
            [resolved, *target[1:], payload_json],
            shell=False,
            timeout=10,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception as exc:
        debug_log({"stage": "forward_failed", "error": str(exc)})


def main() -> None:
    debug_log({"stage": "invoked", "argv": sys.argv[1:], "cwd": os.getcwd()})
    try:
        payload_json = sys.argv[-1] if len(sys.argv) > 1 else "{}"
        payload = json.loads(payload_json)
    except Exception as exc:
        debug_log({"stage": "parse_failed", "error": str(exc), "argv": sys.argv[1:]})
        sys.exit(0)

    try:
        event = codex_notify_to_event(payload, cwd=os.getcwd())
        if event:
            result = post_event(event)
            if result != "ok":
                # Hub 可能尚在启动（桌面端拉起后端的启动竞态）或瞬时抖动：短延时重试一次，
                # 仍失败才跳过转发。总开销 ≤ ~2.5s，符合「不阻塞 Codex」的边界（review HIGH）
                time.sleep(0.5)
                result = post_event(event)
            debug_log({"stage": "posted", "result": result, "payload_type": payload.get("type")})
            # skip forward if post_event failed to avoid split-brain
            if result == "ok":
                forward(payload_json)
        else:
            debug_log({"stage": "skipped", "reason": "converter returned None", "payload_type": payload.get("type"), "payload_keys": sorted(payload.keys())})
    except Exception as exc:
        debug_log({"stage": "error", "error": str(exc)})

    sys.exit(0)


if __name__ == "__main__":
    main()
