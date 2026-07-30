"""Codex notify 钩子 → AI Task Hub 统一事件。

在 codex 的 config.toml 中配置：
    notify = ["python", "D:/develop/AI-Task-Hub/adapters/codex/notify.py"]

Codex 完成一轮任务时会以 JSON 字符串作为最后一个 argv 调用本脚本。
任何异常静默退出 0，绝不阻塞 Codex。
"""

import json
import os
import sys
import urllib.request

# 允许从任意工作目录运行（notify 钩子 cwd 是用户项目目录）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import codex_notify_to_event  # noqa: E402

API_URL = "http://127.0.0.1:17891/api/events"
TIMEOUT_SEC = 2

# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def main() -> None:
    try:
        payload = json.loads(sys.argv[-1]) if len(sys.argv) > 1 else {}
        event = codex_notify_to_event(payload, cwd=os.getcwd())
        if event is None:
            sys.exit(0)
        body = json.dumps(event).encode("utf-8")
        request = urllib.request.Request(
            API_URL,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with _opener.open(request, timeout=TIMEOUT_SEC):
            pass
    except Exception:
        pass
    sys.exit(0)


if __name__ == "__main__":
    main()
