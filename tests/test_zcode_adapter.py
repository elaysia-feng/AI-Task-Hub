"""ZCode Adapter 转换逻辑测试（按路径动态加载，目录名含连字符无法常规 import）。"""

import importlib.util
from pathlib import Path

import pytest

_ADAPTER_PATH = (
    Path(__file__).resolve().parent.parent / "adapters" / "zcode" / "zcode_adapter.py"
)


@pytest.fixture(scope="module")
def adapter():
    spec = importlib.util.spec_from_file_location("zcode_adapter", _ADAPTER_PATH)
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
        "session_id": "zcode-1",
        "cwd": "D:/projects/demo",
        "prompt": "帮我修复登录接口的鉴权问题",
    })

    assert event["source"] == "ZCODE"
    assert event["eventType"] == "TASK_STARTED"
    assert event["externalTaskId"] == "zcode-1"
    assert event["projectPath"] == "D:/projects/demo"
    assert event["title"] == "帮我修复登录接口的鉴权问题"
    assert event["openTarget"] == "terminal"


def test_permission_request_maps_to_needs_input(adapter):
    event = adapter.build_event({
        "hook_event_name": "PermissionRequest",
        "session_id": "zcode-1",
        "cwd": "D:/projects/demo",
        "tool_name": "Bash",
    })

    assert event["eventType"] == "TASK_NEEDS_INPUT"
    assert "等待确认" in event["contentPreview"]
    assert event["title"] == "demo 会话"


def test_stop_maps_to_completed(adapter):
    event = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "zcode-1",
        "cwd": "D:/projects/demo",
    })

    assert event["eventType"] == "TASK_COMPLETED"
    assert event["title"] == "demo 会话"


def test_prompt_title_carries_to_stop_via_cache(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "zcode-cache",
        "cwd": "D:/projects/demo",
        "prompt": "重构数据库层",
    })
    stop = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "zcode-cache",
        "cwd": "D:/projects/demo",
    })
    assert stop["title"] == "重构数据库层"


def test_camel_case_payload_is_supported(adapter):
    """ZCode 兼容层可能给 camelCase 字段，适配器必须同样识别。"""
    event = adapter.build_event({
        "hookEventName": "UserPromptSubmit",
        "sessionId": "zcode-camel",
        "workingDirectory": "D:/projects/demo",
        "prompt": "把构建脚本改成 pnpm",
    })

    assert event is not None
    assert event["eventType"] == "TASK_STARTED"
    assert event["externalTaskId"] == "zcode-camel"
    assert event["projectPath"] == "D:/projects/demo"


def test_turn_id_is_forwarded(adapter):
    event = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "zcode-turn",
        "turn_id": "turn-7",
    })
    assert event["turnId"] == "turn-7"


def test_noise_prompt_does_not_overwrite_good_title(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "zcode-noise",
        "cwd": "D:/projects/demo",
        "prompt": "帮我写交接文档",
    })
    noise = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "zcode-noise",
        "cwd": "D:/projects/demo",
        "prompt": "<task-notification> <task-id>abc</task-id> <to>lead</to>",
    })
    assert noise["title"] == "帮我写交接文档"


def test_long_prompt_truncated(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "zcode-long",
        "prompt": "很长的任务" * 100,
    })
    assert len(event["title"]) <= adapter.MAX_TITLE_LEN + 1
    assert event["title"].endswith("…")


def test_unrelated_hook_returns_none(adapter):
    assert adapter.build_event({"hook_event_name": "PreToolUse"}) is None
    assert adapter.build_event({"hook_event_name": "SessionStart"}) is None
    assert adapter.build_event({}) is None
