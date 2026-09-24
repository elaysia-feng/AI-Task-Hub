"""Codex notify 与提问钩子的事件转换测试。"""

import importlib.util
from pathlib import Path
import sys


def _load_module(name: str, relative_path: str):
    module_path = Path(__file__).resolve().parent.parent / relative_path
    adapter_dir = str(module_path.parent)
    if adapter_dir not in sys.path:
        sys.path.insert(0, adapter_dir)
    spec = importlib.util.spec_from_file_location(name, module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


event_converter = _load_module("codex_event_converter", "adapters/codex/event_converter.py")
prompt_hook = _load_module("codex_prompt_hook", "adapters/codex/prompt_hook.py")


def test_internal_title_generation_turn_is_not_a_task():
    event = event_converter.codex_notify_to_event(
        {
            "type": "agent-turn-complete",
            "session-id": "internal-title-session",
            "input-messages": ["Generate a concise, single-line task title of at most 36 characters"],
            "last-assistant-message": '{"title":"排查 Docker 镜像拉取超时"}',
        },
        cwd="D:/projects/demo",
    )

    assert event is None


def test_user_completion_keeps_stable_session_id_and_prompt_title():
    event = event_converter.codex_notify_to_event(
        {
            "type": "agent-turn-complete",
            "session-id": "user-session",
            "input-messages": ["排查 Docker 镜像拉取超时"],
            "last-assistant-message": "已完成排查。",
        },
        cwd="D:/projects/demo",
    )

    assert event["externalTaskId"] == "user-session"
    assert event["eventType"] == "TASK_COMPLETED"
    assert event["title"] == "排查 Docker 镜像拉取超时"


def test_notify_does_not_use_turn_id_when_session_id_is_missing():
    event = event_converter.codex_notify_to_event(
        {
            "type": "agent-turn-complete",
            "turn-id": "turn-only-id",
            "input-messages": ["普通用户问题"],
        },
        cwd=None,
    )

    assert event is None


def test_prompt_hook_starts_task_before_model_response():
    event = prompt_hook.build_event(
        {
            "hook_event_name": "UserPromptSubmit",
            "session_id": "user-session",
            "cwd": "D:/projects/demo",
            "prompt": "排查 Docker 镜像拉取超时",
        }
    )

    assert event["externalTaskId"] == "user-session"
    assert event["eventType"] == "TASK_STARTED"
    assert event["title"] == "排查 Docker 镜像拉取超时"
