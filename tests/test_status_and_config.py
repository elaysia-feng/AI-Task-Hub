"""阶段 1 新增：/api/status、配置文件优先级、资源解析、日志目录。"""

import os
import sys
from pathlib import Path

from conftest import TEST_DB_NAME
from app.database.mysql import _config_candidates, _load_dotenv, _resource_path
from app.logging_config import log_dir

REPO_ROOT = Path(__file__).resolve().parent.parent


def test_status_endpoint(client):
    res = client.get("/api/status")
    assert res.status_code == 200
    body = res.json()
    assert body["status"] == "ok"
    assert body["version"]
    assert body["db"]["ok"] is True
    backend = body["db"]["backend"]
    assert backend in ("mysql", "sqlite")
    if backend == "sqlite":
        assert body["db"]["database"]  # SQLite：数据库文件路径非空
        assert "host" not in body["db"]
    else:
        assert body["db"]["database"] == TEST_DB_NAME
        assert body["db"]["host"]
        assert body["db"]["port"]
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


def test_config_candidates_frozen_uses_exe_dir(monkeypatch):
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.delenv("AIHUB_CONFIG", raising=False)
    monkeypatch.delenv("APPDATA", raising=False)
    exe_dir = Path(sys.executable).resolve().parent
    candidates = _config_candidates()
    assert candidates[0] == exe_dir / "config.env"
    assert candidates[1] == exe_dir / ".env"
    assert candidates[-1] == REPO_ROOT / ".env"


def test_load_dotenv_strips_quotes(monkeypatch, tmp_path):
    env_file = tmp_path / "config.env"
    env_file.write_text(
        "AIHUB_MYSQL_PASSWORD='dummy@pass#1'\n"
        'AIHUB_MYSQL_DB="ai_task_hub_test2"\n',
        encoding="utf-8",
    )
    monkeypatch.setattr("app.database.mysql._config_candidates", lambda: [env_file])
    monkeypatch.delenv("AIHUB_MYSQL_PASSWORD", raising=False)
    monkeypatch.delenv("AIHUB_MYSQL_DB", raising=False)
    _load_dotenv()
    assert os.environ["AIHUB_MYSQL_PASSWORD"] == "dummy@pass#1"
    assert os.environ["AIHUB_MYSQL_DB"] == "ai_task_hub_test2"


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
