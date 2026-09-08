"""AI 答复提取与端点测试：claude/codex 本地会话记录读取、get_ai_reply 分派、GET /tasks/{id}/ai-reply。"""

import json
from datetime import datetime

import pytest

from app.model.agent_event import AgentEvent
from app.model.task import Task
from app.service import ai_reply
from app.service.ai_reply import (
    ERR_INVALID_ID,
    ERR_MISSING_ID,
    ERR_MISSING_PATH,
    ERR_NO_REPLY,
    ERR_NO_TRANSCRIPT,
    ERR_READ_FAILED,
    ERR_REPLY_TOO_LONG,
    ERR_UNSUPPORTED,
    encode_claude_cwd,
    get_ai_reply,
    read_claude_reply,
    read_codex_reply,
)


@pytest.fixture(autouse=True)
def _clear_cache():
    ai_reply._cache.clear()
    yield
    ai_reply._cache.clear()


def _write(path, lines):
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" 固定行尾：Windows 下默认会把 \n 写成 \r\n，让测试在任意平台确定性复现
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def _task(source="CLAUDE_CODE", external_task_id="s1", project_path="C:/x", **kw):
    return Task(
        id=kw.pop("id", 1),
        source=source,
        external_task_id=external_task_id,
        event_type=kw.pop("event_type", "TASK_STARTED"),
        status=kw.pop("status", "RUNNING"),
        created_at=kw.pop("created_at", datetime.now()),
        project_path=project_path,
    )


def _seed_task(client, **kwargs):
    defaults = {
        "source": "CLAUDE_CODE",
        "eventType": "TASK_COMPLETED",
        "externalTaskId": "seed-1",
        "title": "t",
        "projectPath": "C:/x",
    }
    defaults.update(kwargs)
    return client.app.state.task_service.handle_event(AgentEvent(**defaults))


class TestEncodeCwd:
    def test_windows_path(self):
        assert (
            encode_claude_cwd("C:\\Users\\seele\\Desktop\\AI-Task-Hub")
            == "C--Users-seele-Desktop-AI-Task-Hub"
        )

    def test_posix_path(self):
        assert encode_claude_cwd("D:/projects/demo") == "D--projects-demo"


def _claude_transcript(tmp_path, cwd, session_id, lines):
    p = tmp_path / "projects" / encode_claude_cwd(cwd) / f"{session_id}.jsonl"
    _write(p, lines)
    return p


class TestReadClaudeReply:
    def test_last_assistant_text(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:\\Users\\seele\\Desktop\\AI-Task-Hub"
        _claude_transcript(tmp_path, cwd, "s1", [
            json.dumps({"type": "user", "message": {"content": [{"type": "text", "text": "hi"}]}}),
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "first reply"}]}}),
            json.dumps({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "1", "name": "Bash", "input": {}}]}}),
            # 同一条消息里 text 与 thinking 并存，只取 text
            json.dumps({"type": "assistant", "message": {"content": [
                {"type": "text", "text": "final"},
                {"type": "text", "text": " answer"},
                {"type": "thinking", "thinking": "..."},
            ]}}),
            json.dumps({"type": "summary", "summary": "meta"}),  # 尾部无 message 的元数据行
        ])
        text, err = read_claude_reply(cwd, "s1")
        assert err is None
        assert text == "final\n answer"

    def test_skips_bad_json(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:/x"
        _claude_transcript(tmp_path, cwd, "s1", [
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "ok"}]}}),
            "{ this is not valid json",
        ])
        text, err = read_claude_reply(cwd, "s1")
        assert err is None
        assert text == "ok"

    def test_oversized_latest_reply_not_fallback(self, tmp_path, monkeypatch):
        """超长（>_MAX_LINE_BYTES）的最新行被跳过时，不得回退展示更早的中间过程文本。"""
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        monkeypatch.setattr(ai_reply, "_MAX_LINE_BYTES", 100)
        cwd = "C:/x"
        big_line = json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "X" * 500}]}})
        normal = json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "older reply"}]}})
        _claude_transcript(tmp_path, cwd, "s1", [normal, big_line])  # big 更新，normal 更早
        text, err = read_claude_reply(cwd, "s1")
        assert text is None and err == ERR_REPLY_TOO_LONG

    def test_oversized_older_line_still_ok(self, tmp_path, monkeypatch):
        """超长行若更早、最新行正常，仍正常返回最新答复（不误伤）。"""
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        monkeypatch.setattr(ai_reply, "_MAX_LINE_BYTES", 100)
        cwd = "C:/x"
        big_line = json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "X" * 500}]}})
        normal = json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "newest reply"}]}})
        _claude_transcript(tmp_path, cwd, "s1", [big_line, normal])  # big 更早，normal 最新
        text, err = read_claude_reply(cwd, "s1")
        assert err is None and text == "newest reply"

    def test_traversal_session_id_rejected(self, tmp_path, monkeypatch):
        """external_task_id 可被伪造：含路径分隔符/遍历序列的 id 必须拒绝，不得拼进路径。"""
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        text, err = read_claude_reply("C:/x", "..\\..\\..\\Windows\\win")
        assert text is None and err == ERR_INVALID_ID

    def test_dot_project_path_rejected(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        # cwd=".." 经 encode 后原样保留为父目录，落在 projects 层之外，防御性拒绝
        text, err = read_claude_reply("..", "s1")
        assert text is None and err == ERR_MISSING_PATH

    def test_crlf_multiblock_no_drift(self, tmp_path, monkeypatch):
        """回归：\r\n 行尾 + 行跨 64KB 块边界时，逆序读不得多/丢字符。

        曾因文本模式 seek(字节)/read(字符) 在换行翻译后偏移错位，跨块行被读长一个字符。
        """
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:/x"
        p = tmp_path / "projects" / encode_claude_cwd(cwd) / "s1.jsonl"
        p.parent.mkdir(parents=True, exist_ok=True)
        big = "Y" * 200_000  # 远超 64KB 块，必然跨块
        lines = (
            '{"type":"assistant","message":{"content":[{"type":"text","text":"first"}]}}'
            f'\r\n{{"type":"assistant","message":{{"content":[{{"type":"text","text":"{big}"}}]}}}}\r\n'
            '{"type":"assistant","message":{"content":[{"type":"text","text":"final"}]}}'
        )
        p.write_bytes(lines.encode("utf-8"))
        text, err = read_claude_reply(cwd, "s1")
        assert err is None and text == "final"

    def test_missing_transcript(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        text, err = read_claude_reply("C:/x", "nope")
        assert text is None and err == ERR_NO_TRANSCRIPT

    def test_io_error(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:/x"
        _claude_transcript(tmp_path, cwd, "s1", [
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "x"}]}}),
        ])

        def boom(path):
            raise PermissionError("denied")

        monkeypatch.setattr(ai_reply, "_iter_lines_reverse", boom)
        text, err = read_claude_reply(cwd, "s1")
        assert text is None and err == ERR_READ_FAILED

    def test_truncation(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:/x"
        long = "x" * (ai_reply._MAX_REPLY_CHARS + 100)
        _claude_transcript(tmp_path, cwd, "s1", [
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": long}]}}),
        ])
        text, err = read_claude_reply(cwd, "s1")
        assert err is None
        assert text.endswith("…(截断)")
        assert len(text) == ai_reply._MAX_REPLY_CHARS + len("…(截断)")


def _codex_line(text, phase=None):
    payload = {
        "type": "message",
        "role": "assistant",
        "content": [{"type": "output_text", "text": text}],
    }
    if phase:
        payload["phase"] = phase
    return json.dumps({"timestamp": "2026-08-04T15:17:33Z", "type": "response_item", "payload": payload})


def _codex_rollout(tmp_path, session_id, lines):
    p = (
        tmp_path / "sessions" / "2026" / "08" / "04"
        / f"rollout-2026-08-04T15-17-33-{session_id}.jsonl"
    )
    _write(p, lines)
    return p


class TestReadCodexReply:
    def test_prefers_final_answer(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CODEX_HOME", str(tmp_path))
        session = "019fcba2-83f8-73b3-9478-4dc72d45047f"
        _codex_rollout(tmp_path, session, [
            _codex_line("commentary first"),
            _codex_line("THE FINAL ANSWER", "final_answer"),
        ])
        text, err = read_codex_reply(session)
        assert err is None and text == "THE FINAL ANSWER"

    def test_fallback_last_assistant(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CODEX_HOME", str(tmp_path))
        _codex_rollout(tmp_path, "abc", [
            _codex_line("first reply"),
            _codex_line("latest reply"),
        ])
        text, err = read_codex_reply("abc")
        assert err is None and text == "latest reply"

    def test_ignores_unrelated_rollouts(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CODEX_HOME", str(tmp_path))
        _codex_rollout(tmp_path, "other-session", [_codex_line("wrong")])
        text, err = read_codex_reply("abc")
        assert text is None and err == ERR_NO_TRANSCRIPT

    def test_missing_home(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CODEX_HOME", str(tmp_path / "empty"))
        text, err = read_codex_reply("abc")
        assert text is None and err == ERR_NO_TRANSCRIPT

    def test_traversal_session_id_rejected(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CODEX_HOME", str(tmp_path))
        text, err = read_codex_reply("../../etc/passwd")
        assert text is None and err == ERR_INVALID_ID


class TestGetAiReply:
    def test_claude_missing_id(self):
        result = get_ai_reply(_task(external_task_id=None))
        assert result == {"content": None, "error": ERR_MISSING_ID}

    def test_claude_missing_path(self):
        result = get_ai_reply(_task(external_task_id="s1", project_path=None))
        assert result == {"content": None, "error": ERR_MISSING_PATH}

    def test_claude_ok(self, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        cwd = "C:/x"
        _claude_transcript(tmp_path, cwd, "s1", [
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "reply!"}]}}),
        ])
        result = get_ai_reply(_task(external_task_id="s1", project_path=cwd))
        assert result["content"] == "reply!" and result["error"] is None

    def test_codex_missing_id(self):
        result = get_ai_reply(_task(source="CODEX", external_task_id=None))
        assert result == {"content": None, "error": ERR_MISSING_ID}

    def test_chatgpt_latest_reply(self):
        events = [
            {"payload": {"replyText": "older"}},
            {"payload": {"replyText": ""}},
            {"payload": {"replyText": "newest"}},
        ]
        result = get_ai_reply(_task(source="CHATGPT"), events)
        assert result == {"content": "newest", "error": None}

    def test_chatgpt_no_events(self):
        result = get_ai_reply(_task(source="CHATGPT"), [])
        assert result == {"content": None, "error": ERR_NO_REPLY}

    def test_other_unsupported(self):
        result = get_ai_reply(_task(source="OTHER"))
        assert result == {"content": None, "error": ERR_UNSUPPORTED}


class TestEndpoint:
    def test_404_missing_task(self, client):
        assert client.get("/api/tasks/999999/ai-reply").status_code == 404

    def test_fake_task_traversal_blocked(self, client):
        """伪造事件注入带遍历序列的 external_task_id：ai-reply 拒绝读取，返回错误而非路径逃逸。"""
        ev = AgentEvent(
            source="CLAUDE_CODE",
            eventType="TASK_COMPLETED",
            externalTaskId="..\\..\\..\\Windows\\win",
            projectPath="C:/x",  # 攻击者为读文件必然伪造 projectPath；session_id 必须被拦下
            title="evil",
        )
        resp = client.post("/api/events", json=ev.model_dump(mode="json", by_alias=True))
        assert resp.status_code == 201
        task_id = resp.json()["taskId"]
        r = client.get(f"/api/tasks/{task_id}/ai-reply")
        assert r.status_code == 200
        assert r.json()["content"] is None
        assert r.json()["error"] == ERR_INVALID_ID

    def test_reply_text_truncated_at_ingestion(self):
        """扩展上报的超长 replyText 入库时截断，保护 MySQL JSON 列/时间线渲染/离线队列配额。"""
        from app.api.event_api import _truncate_event

        ev = AgentEvent(
            source="CHATGPT",
            eventType="TASK_COMPLETED",
            externalTaskId="conv-1",
            replyText="x" * 300_000,
        )
        out = _truncate_event(ev)
        assert out.reply_text.endswith("…(truncated)")
        assert len(out.reply_text) == 200_000 + len("…(truncated)")

    def test_claude_ok(self, client, tmp_path, monkeypatch):
        monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
        _claude_transcript(tmp_path, "C:/x", "seed-1", [
            json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "api reply"}]}}),
        ])
        task = _seed_task(client)
        resp = client.get(f"/api/tasks/{task.id}/ai-reply")
        assert resp.status_code == 200
        body = resp.json()
        assert body["taskId"] == task.id
        assert body["source"] == "CLAUDE_CODE"
        assert body["content"] == "api reply"
        assert body["error"] is None

    def test_error_is_null_content_200(self, client, monkeypatch):
        task = _seed_task(client)
        monkeypatch.setattr(
            ai_reply, "get_ai_reply", lambda t, e=None: {"content": None, "error": "boom"}
        )
        resp = client.get(f"/api/tasks/{task.id}/ai-reply")
        assert resp.status_code == 200
        assert resp.json()["content"] is None
        assert resp.json()["error"] == "boom"

    def test_second_call_hits_cache(self, client, monkeypatch):
        task = _seed_task(client)
        calls = []

        def fake(t, e=None):
            calls.append(1)
            return {"content": "cached", "error": None}

        monkeypatch.setattr(ai_reply, "get_ai_reply", fake)
        client.get(f"/api/tasks/{task.id}/ai-reply")
        client.get(f"/api/tasks/{task.id}/ai-reply")
        assert len(calls) == 1

    def test_chatgpt_uses_reply_text(self, client):
        task = _seed_task(
            client,
            source="CHATGPT",
            externalTaskId="conv-1",
            replyText="扩展上报的全文答复",
        )
        resp = client.get(f"/api/tasks/{task.id}/ai-reply")
        assert resp.status_code == 200
        assert resp.json()["content"] == "扩展上报的全文答复"
        assert resp.json()["error"] is None
