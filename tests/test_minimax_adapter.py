"""MiniMax Code 插件钩子适配器转换逻辑测试（按路径动态加载，目录名含连字符无法常规 import）。"""

import importlib.util
from pathlib import Path

import pytest

_ADAPTER_PATH = (
    Path(__file__).resolve().parent.parent / "adapters" / "minimax-code" / "minimax_adapter.py"
)


@pytest.fixture(scope="module")
def adapter():
    spec = importlib.util.spec_from_file_location("minimax_adapter", _ADAPTER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(autouse=True)
def isolated_cache(adapter, monkeypatch, tmp_path):
    """标题缓存优先写 PLUGIN_DATA，测试重定向到临时目录。"""
    monkeypatch.setenv("PLUGIN_DATA", str(tmp_path / "plugin-data"))


def test_prompt_submit_maps_to_started(adapter):
    event = adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "mcode-1",
        "cwd": "D:/projects/demo",
        "prompt": "帮我修复登录接口的鉴权问题",
    })

    assert event["source"] == "MINIMAX_CODE"
    assert event["eventType"] == "TASK_STARTED"
    assert event["externalTaskId"] == "mcode-1"
    assert event["projectPath"] == "D:/projects/demo"
    assert event["title"] == "帮我修复登录接口的鉴权问题"
    assert event["openTarget"] == "terminal"


def test_permission_request_maps_to_needs_input(adapter):
    event = adapter.build_event({
        "hook_event_name": "PermissionRequest",
        "session_id": "mcode-1",
        "cwd": "D:/projects/demo",
        "tool_name": "bash",
    })

    assert event["eventType"] == "TASK_NEEDS_INPUT"
    assert "等待确认" in event["contentPreview"]
    assert event["title"] == "demo 会话"


def test_stop_maps_to_completed(adapter):
    event = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "mcode-1",
        "cwd": "D:/projects/demo",
    })

    assert event["eventType"] == "TASK_COMPLETED"
    assert event["title"] == "demo 会话"


def test_prompt_title_carries_to_stop_via_cache(adapter):
    adapter.build_event({
        "hook_event_name": "UserPromptSubmit",
        "session_id": "mcode-cache",
        "cwd": "D:/projects/demo",
        "prompt": "重构数据库层",
    })
    stop = adapter.build_event({
        "hook_event_name": "Stop",
        "session_id": "mcode-cache",
        "cwd": "D:/projects/demo",
    })
    assert stop["title"] == "重构数据库层"


def test_camel_case_payload_is_supported(adapter):
    event = adapter.build_event({
        "hookEventName": "UserPromptSubmit",
        "sessionId": "mcode-camel",
        "workingDirectory": "D:/projects/demo",
        "prompt": "把构建脚本改成 pnpm",
    })

    assert event is not None
    assert event["eventType"] == "TASK_STARTED"
    assert event["externalTaskId"] == "mcode-camel"


def test_cache_prefers_plugin_data_dir(adapter, tmp_path, monkeypatch):
    monkeypatch.setenv("PLUGIN_DATA", str(tmp_path / "plugin-data"))
    assert adapter._cache_path() == tmp_path / "plugin-data" / "session_titles.json"


def test_cache_falls_back_to_script_dir(adapter, monkeypatch):
    monkeypatch.delenv("PLUGIN_DATA", raising=False)
    assert adapter._cache_path().parent == Path(_ADAPTER_PATH).resolve().parent


def test_unrelated_hook_returns_none(adapter):
    assert adapter.build_event({"hook_event_name": "PreToolUse"}) is None
    assert adapter.build_event({"hook_event_name": "SessionEnd"}) is None
    assert adapter.build_event({}) is None
