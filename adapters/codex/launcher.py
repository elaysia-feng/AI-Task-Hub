"""aihub-codex 启动包装器：包裹 codex CLI，上报完整任务生命周期。

用法：
    python launcher.py <任意 codex 参数>
    例：python launcher.py "帮我修复登录接口"

流程：
    未接入 Hub notify_chain 时，按进程生命周期上报任务；已接入时由 notify_chain
    使用真实会话 ID 记录，避免重复创建任务。

如果 AI Task Hub 的 notify_chain 已安装，由 notify_chain 使用真实会话 ID 记录任务；
本包装器不再另造随机 ID，避免同一次会话出现两条任务。
"""

import ast
import json
import os
import subprocess
import sys
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from event_converter import launcher_event, truncate  # noqa: E402

# 端口允许用 AIHUB_PORT 覆盖（冒烟测试并行实例、端口冲突场景）
API_URL = f"http://127.0.0.1:{int(os.environ.get('AIHUB_PORT', '17891'))}/api/events"
TIMEOUT_SEC = 2
_CODEX_CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".codex", "config.toml")


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


def _hub_notify_installed() -> bool:
    """检查根级 notify 命令是否指向当前适配器的链式转发脚本。"""
    try:
        with open(_CODEX_CONFIG_PATH, encoding="utf-8") as config:
            content = config.read()
        try:
            import tomllib
            codex_config = tomllib.loads(content)
        except ModuleNotFoundError:
            import tomlkit
            codex_config = tomlkit.parse(content)

        command = codex_config.get("notify")
        if isinstance(command, str) or command is None:
            return False
        for argument in command:
            if not isinstance(argument, str) or os.path.basename(argument).lower() != "notify_chain.py":
                continue
            configured_path = os.path.abspath(os.path.expandvars(os.path.expanduser(argument)))
            if not os.path.isfile(configured_path):
                continue
            with open(configured_path, encoding="utf-8") as script:
                script_content = script.read()
            try:
                script_tree = ast.parse(script_content)
            except SyntaxError:
                continue
            imports_hub_converter = any(
                isinstance(node, ast.ImportFrom)
                and node.module == "event_converter"
                and any(alias.name == "codex_notify_to_event" for alias in node.names)
                for node in ast.walk(script_tree)
            )
            posts_events = any(
                isinstance(node, ast.Constant)
                and isinstance(node.value, str)
                and node.value.endswith("/api/events")
                for node in ast.walk(script_tree)
            )
            if imports_hub_converter and posts_events:
                return True
        return False
    except (ImportError, OSError, ValueError, TypeError):
        return False


def main() -> None:
    """转发 Codex CLI 调用；Hub notify 已接入时由会话 hook 负责上报。"""
    args = sys.argv[1:]
    cwd = os.getcwd()
    external_id = f"launcher-{uuid.uuid4().hex[:12]}"
    title = truncate(" ".join(args), 60) if args else "Codex 交互会话"
    report_invocation = not _hub_notify_installed()

    if report_invocation:
        post_event(launcher_event("TASK_STARTED", external_id, cwd, title))

    try:
        process = subprocess.run(["codex", *args])
        event_type = "TASK_COMPLETED" if process.returncode == 0 else "TASK_FAILED"
        if report_invocation:
            post_event(launcher_event(event_type, external_id, cwd, title))
        sys.exit(process.returncode)
    except FileNotFoundError:
        # codex 不在 PATH：必须上报 TASK_FAILED，否则任务在队列里永久僵尸 RUNNING
        print("aihub-codex: error: 找不到 codex 可执行文件（请确认已安装且加入 PATH）", file=sys.stderr)
        if report_invocation:
            post_event(launcher_event("TASK_FAILED", external_id, cwd, title))
        sys.exit(127)


if __name__ == "__main__":
    main()
