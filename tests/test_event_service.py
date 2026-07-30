"""时间线输出契约：camelCase 键 + payload 反序列化（桌面端详情面板依赖）。"""

from app.service.event_service import EventService


class FakeEventRepo:
    def __init__(self, rows):
        self._rows = rows

    def list_by_task(self, task_id):
        return self._rows


def _row(raw_payload, **overrides):
    row = {
        "id": 1,
        "task_id": 7,
        "event_type": "TASK_COMPLETED",
        "raw_payload": raw_payload,
        "created_at": "2026-07-30T10:00:00",
    }
    row.update(overrides)
    return row


def test_shape_converts_to_camel_case_and_parses_payload():
    svc = EventService(FakeEventRepo([_row('{"a": 1}')]))
    (event,) = svc.get_task_timeline(7)
    assert event == {
        "id": 1,
        "taskId": 7,
        "eventType": "TASK_COMPLETED",
        "occurredAt": "2026-07-30T10:00:00",
        "payload": {"a": 1},
    }


def test_shape_invalid_json_falls_back_to_raw():
    svc = EventService(FakeEventRepo([_row("not-json{")]))
    (event,) = svc.get_task_timeline(7)
    assert event["payload"] == {"raw": "not-json{"}


def test_shape_none_payload_becomes_empty_object():
    svc = EventService(FakeEventRepo([_row(None)]))
    (event,) = svc.get_task_timeline(7)
    assert event["payload"] == {}
