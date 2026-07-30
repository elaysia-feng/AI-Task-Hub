"""REST + WebSocket 接口测试。"""

EVENT_PAYLOAD = {
    "source": "CLAUDE_CODE",
    "eventType": "TASK_COMPLETED",
    "externalTaskId": "session-api-001",
    "title": "重构事件服务",
    "projectPath": "D:/develop/AI-Task-Hub",
}


def test_health(client):
    res = client.get("/api/health")
    assert res.status_code == 200
    assert res.json()["status"] == "ok"


def test_full_task_lifecycle(client):
    # 1. Adapter 上报事件
    res = client.post("/api/events", json=EVENT_PAYLOAD)
    assert res.status_code == 201
    task_id = res.json()["taskId"]
    assert isinstance(task_id, int)

    # 2. 出现在未读队列
    queue = client.get("/api/tasks?view=queue").json()["tasks"]
    assert [t["id"] for t in queue] == [task_id]
    assert queue[0]["status"] == "COMPLETED_UNREAD"
    assert queue[0]["source"] == "CLAUDE_CODE"

    # 3. 生命周期时间线可查
    events = client.get(f"/api/tasks/{task_id}/events").json()["events"]
    assert [e["event_type"] for e in events] == ["TASK_COMPLETED"]

    # 4. 标记已读 → 离开队列进入历史
    res = client.post(f"/api/tasks/{task_id}/view")
    assert res.status_code == 200
    assert client.get("/api/tasks?view=queue").json()["tasks"] == []
    history = client.get("/api/tasks?view=history").json()["tasks"]
    assert [t["id"] for t in history] == [task_id]

    # 5. 删除 → 404
    assert client.delete(f"/api/tasks/{task_id}").status_code == 200
    assert client.post(f"/api/tasks/{task_id}/view").status_code == 404


def test_ignore_flow(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    task_id = client.get("/api/tasks?view=queue").json()["tasks"][0]["id"]

    res = client.post(f"/api/tasks/{task_id}/ignore")

    assert res.json()["task"]["status"] == "IGNORED"
    assert client.get("/api/tasks?view=queue").json()["tasks"] == []


def test_clear_all_tasks(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    client.post(
        "/api/events",
        json={**EVENT_PAYLOAD, "externalTaskId": "session-api-002", "eventType": "TASK_FAILED"},
    )
    task_id = client.get("/api/tasks?view=queue").json()["tasks"][0]["id"]

    res = client.delete("/api/tasks")

    assert res.status_code == 200
    assert res.json() == {"success": True, "deleted": 2}
    assert client.get("/api/tasks?view=queue").json()["tasks"] == []
    assert client.get("/api/tasks?view=history").json()["tasks"] == []
    # 事件流水随外键级联删除，任务不存在 → 时间线 404
    assert client.get(f"/api/tasks/{task_id}/events").status_code == 404


def test_websocket_broadcasts_tasks_cleared(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    with client.websocket_connect("/ws/tasks") as ws:
        client.delete("/api/tasks")
        message = ws.receive_json()

    assert message["type"] == "tasks_cleared"
    assert message["deleted"] == 1


def test_read_all_marks_unread_viewed(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    client.post(
        "/api/events",
        json={**EVENT_PAYLOAD, "externalTaskId": "session-api-002", "eventType": "TASK_FAILED"},
    )
    client.post(
        "/api/events",
        json={**EVENT_PAYLOAD, "externalTaskId": "session-api-003", "eventType": "TASK_NEEDS_INPUT"},
    )

    res = client.post("/api/tasks/read-all")

    assert res.status_code == 200
    assert res.json() == {"success": True, "count": 2}
    queue = client.get("/api/tasks?view=queue").json()["tasks"]
    assert [t["status"] for t in queue] == ["NEEDS_INPUT"]  # 等待输入不受一键已读影响
    history = client.get("/api/tasks?view=history").json()["tasks"]
    assert len(history) == 2
    assert all(t["status"] == "VIEWED" for t in history)


def test_websocket_broadcasts_tasks_read_all(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    with client.websocket_connect("/ws/tasks") as ws:
        client.post("/api/tasks/read-all")
        message = ws.receive_json()

    assert message["type"] == "tasks_read_all"
    assert message["count"] == 1


def test_event_validation_rejects_bad_source(client):
    res = client.post("/api/events", json={"source": "SKYNET", "eventType": "TASK_COMPLETED"})
    assert res.status_code == 422


def test_websocket_broadcasts_task_changes(client):
    with client.websocket_connect("/ws/tasks") as ws:
        client.post("/api/events", json=EVENT_PAYLOAD)
        message = ws.receive_json()

    assert message["type"] == "task_changed"
    assert message["eventType"] == "TASK_COMPLETED"
    assert message["task"]["title"] == "重构事件服务"
