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


@pytest.fixture(autouse=True)
def isolated_cache(adapter, monkeypatch, tmp_path):
    """标题缓存写到适配器目录，测试一律重定向到临时目录。"""
    monkeypatch.setattr(adapter, "CACHE_PATH", tmp_path / "session_titles.json")


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
    # 无会话缓存时用项目名作主题，不用通用 waiting 句
    assert event["title"] == "demo 会话"


def test_stop_maps_to_completed(adapter):
    event = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "sess-1",
        "cwd": "D:/projects/demo",
    })

    assert event["eventType"] == "TASK_COMPLETED"
    assert event["title"] == "demo 会话"


def test_prompt_title_carries_to_stop_via_cache(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-cache",
        "cwd": "D:/projects/demo",
        "prompt": "帮我修复登录接口的鉴权问题",
    })
    stop = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "sess-cache",
        "cwd": "D:/projects/demo",
    })
    assert stop["title"] == "帮我修复登录接口的鉴权问题"


def test_notification_prefers_cached_prompt_title(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-cache-2",
        "prompt": "重构数据库层",
    })
    notification = adapter.build_event({
        "hook_event_name": "Notification",
        "session_id": "sess-cache-2",
        "message": "Claude needs your permission to use Bash",
    })
    assert notification["title"] == "重构数据库层"
    assert notification["contentPreview"] == "Claude needs your permission to use Bash"


def test_noise_prompt_does_not_overwrite_good_title(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-noise",
        "cwd": "D:/projects/demo",
        "prompt": "帮我写交接文档",
    })
    noise = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-noise",
        "cwd": "D:/projects/demo",
        "prompt": "<task-notification> <task-id>abc</task-id> <to>lead</to>",
    })
    assert noise["title"] == "帮我写交接文档"
    assert noise["contentPreview"].startswith("<task-notification>")


def test_vite_log_prompt_uses_project_fallback(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-vite",
        "cwd": "C:/Users/wlzx/Desktop/python-report-dev",
        "prompt": "12:08:19 [vite] Internal server error: Unable to parse HTML",
    })
    assert event["title"] == "python-report-dev 会话"


def test_task_notification_summary_extracted(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "sess-xml",
        "cwd": "D:/projects/demo",
        "prompt": "<task-notification><summary>修复登录鉴权</summary></task-notification>",
    })
    assert event["title"] == "修复登录鉴权"


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
