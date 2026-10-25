"""接入集成 API：一键接入幂等性、链式转发保留、旧进程检测、扩展心跳。"""

import json
import os
import sys
import time

import pytest

from app.api import integration_api


@pytest.fixture
def claude_settings(monkeypatch, tmp_path):
    path = tmp_path / "claude" / "settings.json"
    monkeypatch.setattr(integration_api, "CLAUDE_SETTINGS", path)
    return path


@pytest.fixture
def codex_paths(monkeypatch, tmp_path):
    config = tmp_path / "codex" / "config.toml"
    forward = tmp_path / "forward_target.json"
    monkeypatch.setenv("PROGRAMDATA", str(tmp_path / "program-data"))
    monkeypatch.setattr(integration_api, "CODEX_CONFIG", config)
    monkeypatch.setattr(integration_api, "_codex_forward_target", lambda: forward)
    return config, forward


@pytest.fixture
def zcode_config(monkeypatch, tmp_path):
    """ZCode 运行配置目录（安装要求 ~/.zcode/cli 已存在）。"""
    config = tmp_path / "zcode" / "cli" / "config.json"
    config.parent.mkdir(parents=True)
    monkeypatch.setattr(integration_api, "ZCODE_CONFIG", config)
    return config


@pytest.fixture
def minimax_data_dir(monkeypatch, tmp_path):
    """MiniMax Code 数据目录（安装要求目录已存在）。"""
    data_dir = tmp_path / "minimax-data"
    data_dir.mkdir()
    monkeypatch.setenv("MINIMAX_DATA_DIR", str(data_dir))
    monkeypatch.delenv("MAVIS_DATA_DIR", raising=False)
    return data_dir


def test_claude_install_creates_and_idempotent(client, claude_settings):
    res = client.post("/api/integrations/claude-code/install")
    assert res.status_code == 200
    assert res.json() == {"success": True, "changed": True}
    data = json.loads(claude_settings.read_text(encoding="utf-8"))
    # 适配器支持的三类钩子事件全部注册
    for event_name in ("UserPromptSubmit", "Notification", "Stop"):
        assert "claude_adapter.py" in json.dumps(data["hooks"][event_name])

    again = client.post("/api/integrations/claude-code/install").json()
    assert again == {"success": True, "changed": False}


def test_claude_install_preserves_existing_settings(client, claude_settings):
    claude_settings.parent.mkdir(parents=True)
    claude_settings.write_text(json.dumps({"model": "opus", "hooks": {}}), encoding="utf-8")
    client.post("/api/integrations/claude-code/install")
    data = json.loads(claude_settings.read_text(encoding="utf-8"))
    assert data["model"] == "opus"
    assert data["hooks"]["Stop"]


def test_codex_install_chains_existing_notify(client, codex_paths):
    config, forward = codex_paths
    config.parent.mkdir(parents=True)
    # Use forward slashes for cross-platform TOML path (TOML parser handles both / and \)
    config.write_text("notify = ['C:/Tools/old-notify.exe', '--flag']\n", encoding="utf-8")

    res = client.post("/api/integrations/codex/install").json()
    assert res["success"] is True and res["changed"] is True
    assert res["forwardTarget"] is True

    assert json.loads(forward.read_text(encoding="utf-8"))["command"] == ["C:/Tools/old-notify.exe", "--flag"]
    assert "notify_chain.py" in config.read_text(encoding="utf-8")

    again = client.post("/api/integrations/codex/install").json()
    assert again == {"success": True, "changed": False}


def test_codex_install_from_scratch(client, codex_paths):
    config, forward = codex_paths
    res = client.post("/api/integrations/codex/install").json()
    assert res == {"success": True, "changed": True, "forwardTarget": False}
    assert "notify_chain.py" in config.read_text(encoding="utf-8")
    assert not forward.exists()


def test_codex_stale_check_flags_old_processes(client, codex_paths, monkeypatch):
    config, _ = codex_paths
    config.parent.mkdir(parents=True)
    chain = integration_api._codex_chain()
    python = integration_api._adapter_python()
    python_path = python.replace("\\", "/")
    config.write_text(f'notify = ["{python_path}", "{chain.as_posix()}"]\n', encoding="utf-8")
    old = time.time() - 3 * 24 * 3600
    monkeypatch.setattr(
        integration_api, "_codex_processes",
        lambda: [{"pid": 1, "name": "Codex.exe", "createTime": old}],
    )
    body = client.get("/api/integrations/codex/stale-check").json()
    assert body["installed"] is True
    assert body["exeRunning"] is True
    assert body["stale"] is True
    assert body["staleProcesses"][0]["pid"] == 1


def test_codex_process_scan_only_reads_candidate_cmdlines():
    class ProcessError(Exception):
        pass

    class FakeProcess:
        def __init__(self, pid, name, cmdline):
            self.pid = pid
            self.info = {"name": name}
            self._cmdline = cmdline
            self.cmdline_calls = 0

        def cmdline(self):
            self.cmdline_calls += 1
            return self._cmdline

        def create_time(self):
            return 123.0

    unrelated = FakeProcess(1, "python.exe", ["python", "worker.py"])
    codex_node = FakeProcess(2, "node.exe", ["node", "codex"])
    codex_app = FakeProcess(3, "Codex.exe", [])
    processes = [unrelated, codex_node, codex_app]

    class FakePsutil:
        NoSuchProcess = ProcessError
        AccessDenied = ProcessError
        ZombieProcess = ProcessError

        @staticmethod
        def process_iter(attrs):
            assert attrs == ["name"]
            return processes

    found = integration_api._scan_codex_processes(FakePsutil)
    assert {item["pid"] for item in found} == {2, 3}
    assert unrelated.cmdline_calls == 0
    assert codex_node.cmdline_calls == 1
    assert codex_app.cmdline_calls == 0


def test_chatgpt_heartbeat_flow(client, monkeypatch, tmp_path):
    hb_file = tmp_path / "chatgpt_heartbeat.json"
    monkeypatch.setattr(integration_api, "HEARTBEAT_FILE", hb_file)

    before = client.get("/api/integrations/status").json()["chatgpt"]
    assert before["installed"] is False

    res = client.post("/api/integrations/chatgpt/heartbeat", json={"version": "0.2.0"})
    assert res.json() == {"success": True}

    after = client.get("/api/integrations/status").json()["chatgpt"]
    assert after["installed"] is True
    assert after["version"] == "0.2.0"


def test_integrations_status_shape(client):
    body = client.get("/api/integrations/status").json()
    assert set(body) == {"claudeCode", "codex", "chatgpt", "zcode", "minimaxCode", "backend"}
    assert body["backend"]["version"]
    assert "installed" in body["claudeCode"]
    assert "stale" in body["codex"]
    assert set(body["zcode"]) == {"installed", "configPath", "hooksEnabled"}
    assert set(body["minimaxCode"]) == {"installed", "pluginPath", "dataDir"}


def test_chatgpt_extension_dir_dev_points_to_repo(client, monkeypatch):
    """开发态（未冻结）：扩展目录直接指向仓库源码，不做物化。"""
    # 确保不处于冻结态（隔离其他用例的 monkeypatch 残留）
    monkeypatch.delattr(sys, "_MEIPASS", raising=False)
    if getattr(sys, "frozen", False):
        monkeypatch.setattr(sys, "frozen", False)

    body = client.get("/api/integrations/status").json()
    assert body["chatgpt"]["extensionDir"] == str(
        integration_api._REPO_ROOT / "adapters" / "chatgpt-extension"
    )


def test_chatgpt_extension_dir_frozen_materializes(client, monkeypatch, tmp_path):
    """打包态（sys.frozen + _MEIPASS）：扩展被物化到持久用户目录，status 返回该路径。"""
    bundled = tmp_path / "bundled"
    ext_source = bundled / "adapters" / "chatgpt-extension"
    ext_source.mkdir(parents=True)
    (ext_source / "manifest.json").write_text('{"name": "ai-task-hub"}', encoding="utf-8")
    (ext_source / "background.js").write_text("console.log('hub')", encoding="utf-8")

    user_base = tmp_path / "userdata"
    monkeypatch.setattr(integration_api, "user_data_dir", lambda: user_base)
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "_MEIPASS", str(bundled), raising=False)

    body = client.get("/api/integrations/status").json()
    expected = user_base / "chatgpt-extension"
    assert body["chatgpt"]["extensionDir"] == str(expected)
    assert (expected / "manifest.json").read_text(encoding="utf-8") == '{"name": "ai-task-hub"}'
    assert (expected / "background.js").read_text(encoding="utf-8") == "console.log('hub')"

    # 幂等：再次调用不报错，文件仍在
    body2 = client.get("/api/integrations/status").json()
    assert body2["chatgpt"]["extensionDir"] == str(expected)
    assert (expected / "manifest.json").exists()


def _freeze(monkeypatch, tmp_path):
    """模拟 PyInstaller 冻结态：构造 _MEIPASS 捆绑目录 + 用户数据目录。"""
    bundled = tmp_path / "bundled"
    user_base = tmp_path / "userdata"
    # claude-code 适配器（claude_adapter.py 与其 session_titles.json 须同目录）
    cc = bundled / "adapters" / "claude-code"
    cc.mkdir(parents=True)
    (cc / "claude_adapter.py").write_text("print('adapter')", encoding="utf-8")
    (cc / "session_titles.json").write_text("{}", encoding="utf-8")
    # codex 适配器 + 开发态运行时产物（物化时应排除 forward_target.json）
    cx = bundled / "adapters" / "codex"
    cx.mkdir(parents=True)
    (cx / "notify_chain.py").write_text("print('chain')", encoding="utf-8")
    (cx / "prompt_hook.py").write_text("print('prompt hook')", encoding="utf-8")
    (cx / "forward_target.json").write_text('{"command":["dev-only"]}', encoding="utf-8")
    monkeypatch.setattr(integration_api, "user_data_dir", lambda: user_base)
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "_MEIPASS", str(bundled), raising=False)
    return user_base


def test_frozen_install_materializes_adapters(client, claude_settings, codex_paths, monkeypatch, tmp_path):
    """打包态：install 把适配器物化到用户目录，钩子/notify 指向物化路径；运行时产物不复制。"""
    user_base = _freeze(monkeypatch, tmp_path)
    # 打包态模拟解释器：相对名 "python" 在测试环境 PATH 上不存在，
    # 用当前 pytest 解释器绝对路径（sys.executable 任何平台都存在）
    py = sys.executable
    monkeypatch.setenv("AIHUB_PYTHON", py)

    res = client.post("/api/integrations/claude-code/install").json()
    assert res["success"] is True and res["changed"] is True
    cc_dir = user_base / "adapters" / "claude-code"
    assert (cc_dir / "claude_adapter.py").exists()
    assert (cc_dir / "session_titles.json").exists()
    data = json.loads(claude_settings.read_text(encoding="utf-8"))
    cmd = data["hooks"]["Stop"][0]["hooks"][0]["command"]
    assert str(cc_dir / "claude_adapter.py") in cmd
    assert cmd.startswith(f'"{py}"')

    config, forward = codex_paths
    res = client.post("/api/integrations/codex/install").json()
    assert res["success"] is True
    cx_dir = user_base / "adapters" / "codex"
    assert (cx_dir / "notify_chain.py").exists()
    # 物化排除运行时产物 forward_target.json（install 时才按需写入用户目录）
    assert not (cx_dir / "forward_target.json").exists()
    assert "notify_chain.py" in config.read_text(encoding="utf-8")


def test_frozen_install_without_python_returns_error(client, codex_paths, monkeypatch, tmp_path):
    """打包态找不到 Python 时：install 返回明确错误，不写坏配置。"""
    _freeze(monkeypatch, tmp_path)
    monkeypatch.delenv("AIHUB_PYTHON", raising=False)
    monkeypatch.setattr(integration_api.shutil, "which", lambda name: None)

    res = client.post("/api/integrations/codex/install").json()
    assert res["success"] is False
    assert "Python" in res["error"]
    config, _ = codex_paths
    assert not config.exists()  # 未写入 config.toml


def test_adapter_python_dev_uses_venv(monkeypatch):
    """开发态：_adapter_python 返回仓库 .venv 的解释器路径（跨平台）。"""
    monkeypatch.delattr(sys, "frozen", raising=False)
    scripts = "Scripts" if sys.platform == "win32" else "bin"
    exe = "python.exe" if sys.platform == "win32" else "python"
    expected = str(integration_api._REPO_ROOT / ".venv" / scripts / exe)
    assert integration_api._adapter_python() == expected


def test_adapter_python_frozen_rejects_nonexistent_absolute(monkeypatch, tmp_path):
    """打包态 AIHUB_PYTHON 指向不存在的绝对路径 → None（不把坏命令写进配置）。"""
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setenv("AIHUB_PYTHON", str(tmp_path / "no-such-python.exe"))
    assert integration_api._adapter_python() is None


def test_adapter_python_frozen_resolves_relative_via_path(monkeypatch, tmp_path):
    """打包态 AIHUB_PYTHON 为相对名：PATH 上找不到 → None。"""
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setenv("AIHUB_PYTHON", "no-such-interpreter-on-path")
    monkeypatch.setattr(integration_api.shutil, "which", lambda name: None)
    assert integration_api._adapter_python() is None


def test_frozen_install_missing_adapter_returns_error(client, claude_settings, monkeypatch, tmp_path):
    """打包态捆绑缺少适配器脚本：install 返回明确错误而非写坏配置（fail-open 修复）。"""
    bundled = tmp_path / "bundled"
    user_base = tmp_path / "userdata"
    (bundled / "adapters" / "claude-code").mkdir(parents=True)  # 目录在但缺 claude_adapter.py
    monkeypatch.setattr(integration_api, "user_data_dir", lambda: user_base)
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "_MEIPASS", str(bundled), raising=False)
    monkeypatch.setenv("AIHUB_PYTHON", sys.executable)

    res = client.post("/api/integrations/claude-code/install").json()
    assert res["success"] is False
    assert "适配器" in res["error"]
    assert not claude_settings.exists()  # 未写入 settings.json


# ---------- ZCode ----------

def test_zcode_install_writes_hooks_and_is_idempotent(client, zcode_config):
    res = client.post("/api/integrations/zcode/install").json()
    assert res == {"success": True, "changed": True}

    data = json.loads(zcode_config.read_text(encoding="utf-8"))
    # ZCode 配置型钩子默认关闭，必须显式打开
    assert data["hooks"]["enabled"] is True
    for event_name in ("UserPromptSubmit", "PermissionRequest", "Stop"):
        entries = data["hooks"]["events"][event_name]
        assert len(entries) == 1
        hook = entries[0]["hooks"][0]
        assert hook["type"] == "process"
        assert "zcode_adapter.py" in hook["args"][0]
        # ZCode 的 schema 严格：条目只允许 matcher/hooks，钩子只允许这几个键
        assert set(entries[0]) == {"hooks"}
        assert set(hook) == {"type", "command", "args", "timeoutMs"}

    again = client.post("/api/integrations/zcode/install").json()
    assert again == {"success": True, "changed": False}


def test_zcode_install_preserves_other_config_and_third_party_hooks(client, zcode_config):
    zcode_config.write_text(json.dumps({
        "ui": {"locale": "zh-CN"},
        "hooks": {
            "enabled": True,
            "events": {
                "PreToolUse": [{"hooks": [{"type": "process", "command": "guard", "args": ["guard.js"]}]}],
                "UserPromptSubmit": [{"hooks": [{"type": "process", "command": "other", "args": ["other.js"]}]}],
            },
        },
    }), encoding="utf-8")

    assert client.post("/api/integrations/zcode/install").json()["success"] is True
    data = json.loads(zcode_config.read_text(encoding="utf-8"))
    assert data["ui"] == {"locale": "zh-CN"}
    # 第三方钩子原样保留
    assert data["hooks"]["events"]["PreToolUse"][0]["hooks"][0]["command"] == "guard"
    assert data["hooks"]["events"]["UserPromptSubmit"][0]["hooks"][0]["command"] == "other"
    # 本应用自己的条目追加在后，不覆盖第三方条目
    ours = [e for e in data["hooks"]["events"]["UserPromptSubmit"] if "zcode_adapter.py" in json.dumps(e)]
    assert len(ours) == 1


def test_zcode_install_keeps_user_disabled_switch(client, zcode_config):
    """用户显式关闭 hooks.enabled 时保留该设置（这是 ZCode 的全局开关）。"""
    zcode_config.write_text(json.dumps({"hooks": {"enabled": False}}), encoding="utf-8")

    assert client.post("/api/integrations/zcode/install").json()["success"] is True
    data = json.loads(zcode_config.read_text(encoding="utf-8"))
    assert data["hooks"]["enabled"] is False

    status = client.get("/api/integrations/status").json()["zcode"]
    assert status["hooksEnabled"] is False
    assert status["installed"] is True  # 钩子已登记，等用户打开开关即可生效


def test_zcode_install_refuses_broken_config(client, zcode_config):
    zcode_config.write_text("{ broken", encoding="utf-8")
    res = client.post("/api/integrations/zcode/install").json()
    assert res["success"] is False
    assert "解析失败" in res["error"]
    assert zcode_config.read_text(encoding="utf-8") == "{ broken"  # 未修改


def test_zcode_install_requires_zcode_directory(client, monkeypatch, tmp_path):
    missing = tmp_path / "no-zcode" / "cli" / "config.json"
    monkeypatch.setattr(integration_api, "ZCODE_CONFIG", missing)
    res = client.post("/api/integrations/zcode/install").json()
    assert res["success"] is False
    assert "未检测到 ZCode" in res["error"]
    assert not missing.exists()


def test_zcode_status_reports_installed_after_install(client, zcode_config):
    before = client.get("/api/integrations/status").json()["zcode"]
    assert before["installed"] is False
    assert before["hooksEnabled"] is False

    client.post("/api/integrations/zcode/install")
    after = client.get("/api/integrations/status").json()["zcode"]
    assert after["installed"] is True
    assert after["hooksEnabled"] is True


# ---------- MiniMax Code ----------

def test_minimax_install_creates_plugin_and_is_idempotent(client, minimax_data_dir):
    res = client.post("/api/integrations/minimax-code/install").json()
    assert res == {"success": True, "changed": True}

    plugin_dir = minimax_data_dir / "plugins" / "ai-task-hub-bridge"
    assert (plugin_dir / ".minimax-plugin" / "plugin.json").is_file()
    assert (plugin_dir / "hooks" / "hooks.json").is_file()
    assert (plugin_dir / "scripts" / "minimax_adapter.py").is_file()
    assert (plugin_dir / "icon.png").is_file()
    assert (plugin_dir / ".aihub-managed.json").is_file()

    hooks = json.loads((plugin_dir / "hooks" / "hooks.json").read_text(encoding="utf-8"))
    for event_name in ("UserPromptSubmit", "PermissionRequest", "Stop"):
        handler = hooks["hooks"][event_name][0]["hooks"][0]
        assert handler["type"] == "command"
        assert 1 <= handler["timeout"] <= 10
        assert "minimax_adapter.py" in handler["command"]
        assert "minimax_adapter.py" in handler["commandWindows"]

    again = client.post("/api/integrations/minimax-code/install").json()
    assert again == {"success": True, "changed": False}


def test_minimax_install_refuses_foreign_directory(client, minimax_data_dir):
    plugin_dir = minimax_data_dir / "plugins" / "ai-task-hub-bridge"
    plugin_dir.mkdir(parents=True)
    (plugin_dir / "notes.txt").write_text("user content", encoding="utf-8")

    res = client.post("/api/integrations/minimax-code/install").json()
    assert res["success"] is False
    assert "未修改" in res["error"]
    assert not (plugin_dir / "hooks" / "hooks.json").exists()
    assert (plugin_dir / "notes.txt").is_file()  # 用户内容未被删除


def test_minimax_install_requires_data_dir(client, monkeypatch, tmp_path):
    monkeypatch.setenv("MINIMAX_DATA_DIR", str(tmp_path / "missing"))
    monkeypatch.delenv("MAVIS_DATA_DIR", raising=False)
    res = client.post("/api/integrations/minimax-code/install").json()
    assert res["success"] is False
    assert "未检测到 MiniMax Code" in res["error"]


def test_minimax_status_reports_installed_after_install(client, minimax_data_dir):
    before = client.get("/api/integrations/status").json()["minimaxCode"]
    assert before["installed"] is False

    client.post("/api/integrations/minimax-code/install")
    after = client.get("/api/integrations/status").json()["minimaxCode"]
    assert after["installed"] is True
    assert after["dataDir"] == str(minimax_data_dir)
    assert after["pluginPath"].endswith("ai-task-hub-bridge")
