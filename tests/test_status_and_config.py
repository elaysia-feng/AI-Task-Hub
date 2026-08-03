"""阶段 1 新增：/api/status、配置文件优先级、资源解析、日志目录。"""

import sys
from pathlib import Path

from conftest import TEST_DB_NAME
from app.database.mysql import _config_candidates, _resource_path
from app.logging_config import log_dir

REPO_ROOT = Path(__file__).resolve().parent.parent


def test_status_endpoint(client):
    res = client.get("/api/status")
    assert res.status_code == 200
    body = res.json()
    assert body["status"] == "ok"
    assert body["version"]
    assert body["db"]["ok"] is True
    assert body["db"]["database"] == TEST_DB_NAME
    assert isinstance(body["tasks"], int)
    assert isinstance(body["events"], int)
    assert body["logFile"].endswith("backend.log")
    assert body["uptimeSec"] >= 0


def test_config_candidates_priority(monkeypatch, tmp_path):
    explicit = tmp_path / "explicit.env"
    explicit.write_text("")  # must exist for AIHUB_CONFIG to be accepted
    monkeypatch.setenv("AIHUB_CONFIG", str(explicit))
    monkeypatch.setenv("APPDATA", str(tmp_path))
    candidates = _config_candidates()
    assert candidates[0] == explicit
    assert candidates[1] == tmp_path / "AI Task Hub" / "config.env"
    assert candidates[-1] == REPO_ROOT / ".env"


def test_config_candidates_without_explicit(monkeypatch, tmp_path):
    monkeypatch.delenv("AIHUB_CONFIG", raising=False)
    monkeypatch.setenv("APPDATA", str(tmp_path))
    candidates = _config_candidates()
    assert candidates[0] == tmp_path / "AI Task Hub" / "config.env"


def test_resource_path_source_layout():
    schema = _resource_path("app/database/schema.sql")
    assert schema == REPO_ROOT / "app" / "database" / "schema.sql"
    assert schema.exists()


def test_log_dir_frozen_uses_appdata(monkeypatch, tmp_path):
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setenv("APPDATA", str(tmp_path))
    path = log_dir()
    assert path == tmp_path / "AI Task Hub" / "logs"
    assert path.is_dir()
