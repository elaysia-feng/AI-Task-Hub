"""跨模块共享常量。

桌面端、事件服务与 Adapter 之间的连线契约以本文件为准。
Adapter 无法 import 本包时，以 shared/event_schema.json 为协议文档。
"""

import os

APP_NAME = "AI Task Hub"
APP_ID = "ai-task-hub"
APP_VERSION = "0.1.10"

API_HOST = "127.0.0.1"
# 端口可被 AIHUB_PORT 覆盖（冒烟测试并行实例、端口冲突场景）
API_PORT = int(os.environ.get("AIHUB_PORT", "17891"))
API_BASE_URL = f"http://{API_HOST}:{API_PORT}"
WS_URL = f"ws://{API_HOST}:{API_PORT}/ws/tasks"

# Queue 页面展示的状态（未读队列）
QUEUE_STATUSES = ("RUNNING", "NEEDS_INPUT", "COMPLETED_UNREAD", "FAILED_UNREAD")
# History 页面展示的状态
HISTORY_STATUSES = ("VIEWED", "IGNORED")
# 一键已读的作用范围（NEEDS_INPUT 仍需用户处理，不在其列）
UNREAD_STATUSES = ("COMPLETED_UNREAD", "FAILED_UNREAD")
# 全部任务状态（per-status 分页与 summary 的合法取值集合）
ALL_STATUSES = QUEUE_STATUSES + HISTORY_STATUSES

SOURCE_LABELS = {
    "CHATGPT": "ChatGPT",
    "CLAUDE_CODE": "Claude Code",
    "CODEX": "Codex",
    "OTHER": "其他",
}
