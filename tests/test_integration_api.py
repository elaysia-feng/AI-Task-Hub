"""接入集成 API：一键接入幂等性、链式转发保留、旧进程检测、扩展心跳。"""

import json
import os
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
    monkeypatch.setattr(integration_api, "CODEX_CONFIG", config)
    monkeypatch.setattr(integration_api, "CODEX_FORWARD_TARGET", forward)
    return config, forward


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
    config.write_text('notify = ["notify_chain.py"]\n', encoding="utf-8")
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
    assert set(body) == {"claudeCode", "codex", "chatgpt", "backend"}
    assert body["backend"]["version"]
    assert "installed" in body["claudeCode"]
    assert "stale" in body["codex"]
