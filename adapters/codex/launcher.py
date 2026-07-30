"""aihub-codex 启动包装器：包裹 codex CLI，上报完整任务生命周期。

用法：
    python launcher.py <任意 codex 参数>
    例：python launcher.py "帮我修复登录接口"

流程：
    记录项目路径 → 上报 TASK_STARTED → 透传启动 codex → 进程结束
    → 按退出码上报 TASK_COMPLETED / TASK_FAILED。

如果已在 codex config.toml 配置了 notify 钩子，完成事件以 notify 为准
（notify 携带真实会话 ID，本包装器的事件使用本地生成的 ID，两条任务互不影响）。
"""

import json
import os
import subprocess
import sys
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import launcher_event, truncate  # noqa: E402

API_URL = "http://127.0.0.1:17891/api/events"
TIMEOUT_SEC = 2


# 本机可能开启系统代理（Clash 等）：localhost 请求必须直连
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def post_event(event: dict) -> None:
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
    except Exception:
        # 事件服务未启动不影响 codex 正常运行
        pass


def main() -> None:
    args = sys.argv[1:]
    cwd = os.getcwd()
    external_id = f"launcher-{uuid.uuid4().hex[:12]}"
    title = truncate(" ".join(args), 60) if args else "Codex 交互会话"

    post_event(launcher_event("TASK_STARTED", external_id, cwd, title))

    process = subprocess.run(["codex", *args])
    event_type = "TASK_COMPLETED" if process.returncode == 0 else "TASK_FAILED"
    post_event(launcher_event(event_type, external_id, cwd, title))

    sys.exit(process.returncode)


if __name__ == "__main__":
    main()
