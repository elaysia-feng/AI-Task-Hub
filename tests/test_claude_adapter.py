"""Claude Code Adapter 转换逻辑测试（按路径动态加载，目录名含连字符无法常规 import）。"""

import importlib.util
from pathlib import Path

import pytest

_ADAPTER_PATH = (
    Path(__file__).resolve().parent.parent / "adapters" / "claude-code" / "claude_adapter.py"
)


@pytest.fixture(scope="module")
def adapter():
    spec = importlib.util.spec_from_file_location("claude_adapter", _ADAPTER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_prompt_submit_maps_to_started(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-1",
        "cwd": "D:/projects/demo",
        "prompt": "帮我修复登录接口的鉴权问题",
    })

    assert event["source"] == "CLAUDE_CODE"
    assert event["eventType"] == "TASK_STARTED"
    assert event["externalTaskId"] == "sess-1"
    assert event["projectPath"] == "D:/projects/demo"
    assert event["title"] == "帮我修复登录接口的鉴权问题"
    assert event["openTarget"] == "terminal"


def test_notification_maps_to_needs_input(adapter):
    event = adapter.build_event({
        "hook_event_name": "Notification",
        "session_id": "sess-1",
        "cwd": "D:/projects/demo",
        "message": "Claude needs your permission to use Bash",
    })

    assert event["eventType"] == "TASK_NEEDS_INPUT"
    assert event["contentPreview"] == "Claude needs your permission to use Bash"


def test_stop_maps_to_completed(adapter):
    event = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "sess-1",
        "cwd": "D:/projects/demo",
    })

    assert event["eventType"] == "TASK_COMPLETED"


def test_long_prompt_truncated(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "s",
        "prompt": "很长的任务" * 100,
    })

    assert len(event["title"]) <= adapter.MAX_TITLE_LEN + 1
    assert event["title"].endswith("…")


def test_unknown_hook_returns_none(adapter):
    assert adapter.build_event({"hook_event_name": "PreToolUse"}) is None
    assert adapter.build_event({}) is None
