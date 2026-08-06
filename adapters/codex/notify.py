"""Codex notify 钩子 → AI Task Hub 统一事件。

在 codex 的 config.toml 中配置：
    notify = ["python", "D:/develop/AI-Task-Hub/adapters/codex/notify.py"]

Codex 完成一轮任务时会以 JSON 字符串作为最后一个 argv 调用本脚本。
任何异常静默退出 0，绝不阻塞 Codex。
"""

import json
import logging
import os
import sys
import urllib.request
from datetime import datetime
from logging.handlers import RotatingFileHandler

# 允许从任意工作目录运行（notify 钩子 cwd 是用户项目目录）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import codex_notify_to_event  # noqa: E402

# 端口允许用 AIHUB_PORT 覆盖（冒烟测试并行实例、端口冲突场景）
API_URL = f"http://127.0.0.1:{int(os.environ.get('AIHUB_PORT', '17891'))}/api/events"
TIMEOUT_SEC = 2
# 与 notify_chain 共用排障日志，方便统一排查（review HIGH：原实现静默吞掉全部异常）
DEBUG_LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notify_debug.log")

# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

_debug_logger = None


def debug_log(entry: dict) -> None:
    """排障日志：记录每次 notify 触发的载荷与处理结果，任何失败静默（不阻塞 Codex）。"""
    global _debug_logger
    try:
        entry.setdefault("ts", datetime.now().isoformat(timespec="seconds"))
        if _debug_logger is None:
            _debug_logger = logging.getLogger("notify_debug_standalone")
            _debug_logger.setLevel(logging.INFO)
            _debug_logger.addHandler(
                RotatingFileHandler(
                    DEBUG_LOG_PATH, maxBytes=1_000_000, backupCount=3, encoding="utf-8"
                )
            )
            _debug_logger.handlers[0].setFormatter(logging.Formatter("%(message)s"))
        _debug_logger.info(json.dumps(entry, ensure_ascii=False))
    except Exception:
        pass


def main() -> None:
    debug_log({"stage": "invoked", "argv": sys.argv[1:], "cwd": os.getcwd()})
    try:
        payload = json.loads(sys.argv[-1]) if len(sys.argv) > 1 else {}
        event = codex_notify_to_event(payload, cwd=os.getcwd())
    except Exception as exc:
        # 载荷非 JSON / 转换异常：记日志后照常退出 0，不阻塞 Codex
        debug_log({"stage": "error", "error": str(exc), "argv": sys.argv[1:]})
        sys.exit(0)
    if event is None:
        debug_log({"stage": "skipped", "reason": "converter returned None", "payload_type": payload.get("type")})
        sys.exit(0)
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
        debug_log({"stage": "posted", "result": "ok", "payload_type": payload.get("type")})
    except Exception as exc:
        # 网络不可达 / Hub 未启动：记录失败原因，便于事后排查而非完全隐形
        debug_log({"stage": "post_failed", "error": str(exc)})
    sys.exit(0)


if __name__ == "__main__":
    main()
