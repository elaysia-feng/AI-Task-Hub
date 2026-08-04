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
    assert client.get(f"/api/tasks/{task_id}").json()["task"]["id"] == task_id

    # 3. 生命周期时间线可查（桌面端契约：camelCase + payload 对象）
    events = client.get(f"/api/tasks/{task_id}/events").json()["events"]
    assert [e["eventType"] for e in events] == ["TASK_COMPLETED"]
    assert events[0]["taskId"] == task_id
    assert isinstance(events[0]["payload"], dict)
    assert events[0]["occurredAt"]

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
    events = client.get(f"/api/tasks/{task_id}/events").json()["events"]
    assert [event["eventType"] for event in events] == ["TASK_COMPLETED", "TASK_IGNORED"]


def test_clear_all_tasks(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    client.post(
        "/api/events",
        json={**EVENT_PAYLOAD, "externalTaskId": "session-api-002", "eventType": "TASK_FAILED"},
    )
    task_id = client.get("/api/tasks?view=queue").json()["tasks"][0]["id"]

    res = client.delete("/api/tasks?confirm=true")

    assert res.status_code == 200
    assert res.json() == {"success": True, "deleted": 2}
    assert client.get("/api/tasks?view=queue").json()["tasks"] == []
    assert client.get("/api/tasks?view=history").json()["tasks"] == []
    # 事件流水随外键级联删除，任务不存在 → 时间线 404
    assert client.get(f"/api/tasks/{task_id}/events").status_code == 404


def test_websocket_broadcasts_tasks_cleared(client):
    client.post("/api/events", json=EVENT_PAYLOAD)
    with client.websocket_connect("/ws/tasks") as ws:
        client.delete("/api/tasks?confirm=true")
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


def test_list_tasks_pagination(client):
    """分页：limit/offset 生效、hasMore 正确、翻页无重复无遗漏。"""
    ids: list[int] = []
    for i in range(5):
        res = client.post(
            "/api/events",
            json={
                **EVENT_PAYLOAD,
                "externalTaskId": f"page-session-{i}",
                "title": f"分页任务 {i}",
            },
        )
        ids.append(res.json()["taskId"])
    for task_id in ids:
        client.post(f"/api/tasks/{task_id}/view")  # 全部已读 → 进入历史

    page1 = client.get("/api/tasks?view=history&limit=2").json()
    assert len(page1["tasks"]) == 2
    assert page1["hasMore"] is True

    page2 = client.get("/api/tasks?view=history&limit=2&offset=2").json()
    assert len(page2["tasks"]) == 2
    assert page2["hasMore"] is True

    page3 = client.get("/api/tasks?view=history&limit=2&offset=4").json()
    assert len(page3["tasks"]) == 1
    assert page3["hasMore"] is False

    seen = {t["id"] for t in page1["tasks"] + page2["tasks"] + page3["tasks"]}
    assert seen == set(ids)  # 三页正好覆盖全部 5 条，无重复

    # limit 超上限被校验拦截
    assert client.get("/api/tasks?view=history&limit=501").status_code == 422


def test_list_tasks_by_status_pagination(client):
    """按状态（种类）独立分页：每种状态一条流，翻页互不串扰。"""
    viewed_ids: list[int] = []
    for i in range(5):
        res = client.post(
            "/api/events",
            json={
                **EVENT_PAYLOAD,
                "externalTaskId": f"by-status-viewed-{i}",
                "title": f"已读任务 {i}",
            },
        )
        viewed_ids.append(res.json()["taskId"])
    ignored_ids: list[int] = []
    for i in range(3):
        res = client.post(
            "/api/events",
            json={
                **EVENT_PAYLOAD,
                "externalTaskId": f"by-status-ignored-{i}",
                "title": f"忽略任务 {i}",
            },
        )
        ignored_ids.append(res.json()["taskId"])
    for task_id in viewed_ids:
        client.post(f"/api/tasks/{task_id}/view")
    for task_id in ignored_ids:
        client.post(f"/api/tasks/{task_id}/ignore")

    # VIEWED 一条独立分页流：3 页正好覆盖 5 条，无重复
    page1 = client.get("/api/tasks?status=VIEWED&limit=2").json()
    assert len(page1["tasks"]) == 2
    assert page1["hasMore"] is True
    page2 = client.get("/api/tasks?status=VIEWED&limit=2&offset=2").json()
    assert page2["hasMore"] is True
    page3 = client.get("/api/tasks?status=VIEWED&limit=2&offset=4").json()
    assert len(page3["tasks"]) == 1
    assert page3["hasMore"] is False
    seen = {t["id"] for t in page1["tasks"] + page2["tasks"] + page3["tasks"]}
    assert seen == set(viewed_ids)
    assert all(t["status"] == "VIEWED" for t in page1["tasks"] + page2["tasks"] + page3["tasks"])

    # IGNORED 独立流：offset 从 0 开始，不受 VIEWED 分页影响（按 id DESC 取最新的 2 条）
    ignored_page = client.get("/api/tasks?status=IGNORED&limit=2").json()
    assert len(ignored_page["tasks"]) == 2
    assert ignored_page["hasMore"] is True
    assert all(t["status"] == "IGNORED" for t in ignored_page["tasks"])
    assert {t["id"] for t in ignored_page["tasks"]} <= set(ignored_ids)

    # 未知状态被校验拦截
    assert client.get("/api/tasks?status=SKYNET").status_code == 422


def test_tasks_summary_counts_by_status(client):
    """summary 返回 6 种状态准确计数（GROUP BY 一条查询），缺失状态补 0。"""
    client.post("/api/events", json={**EVENT_PAYLOAD, "externalTaskId": "sum-running", "eventType": "TASK_STARTED"})
    client.post(
        "/api/events",
        json={**EVENT_PAYLOAD, "externalTaskId": "sum-input", "eventType": "TASK_NEEDS_INPUT"},
    )
    completed = client.post(
        "/api/events", json={**EVENT_PAYLOAD, "externalTaskId": "sum-done", "eventType": "TASK_COMPLETED"}
    ).json()["taskId"]
    client.post(
        "/api/events", json={**EVENT_PAYLOAD, "externalTaskId": "sum-failed", "eventType": "TASK_FAILED"}
    )
    client.post(f"/api/tasks/{completed}/view")  # COMPLETED_UNREAD → VIEWED

    counts = client.get("/api/tasks/summary").json()["counts"]

    assert counts == {
        "RUNNING": 1,
        "NEEDS_INPUT": 1,
        "COMPLETED_UNREAD": 0,  # 已读离开队列
        "FAILED_UNREAD": 1,
        "VIEWED": 1,
        "IGNORED": 0,
    }
