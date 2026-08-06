-- AI Task Hub SQLite Schema
-- 主键：INTEGER PRIMARY KEY AUTOINCREMENT；时间字段：TEXT（ISO-8601，存本地时间）
-- (source, external_task_id_not_null) 唯一约束用于跨平台事件幂等去重；NULL/空串经
-- generated column 归一到 ''，同源 NULL/空串事件合并去重到同一条任务（非「每次新建」）

CREATE TABLE IF NOT EXISTS task (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source TEXT NOT NULL,
    external_task_id TEXT,
    event_type TEXT NOT NULL,
    title TEXT,
    content_preview TEXT,
    project_path TEXT,
    open_target TEXT,
    open_url TEXT,
    status TEXT NOT NULL,
    created_at TEXT NOT NULL,
    completed_at TEXT,
    viewed_at TEXT,
    external_task_id_not_null TEXT GENERATED ALWAYS AS (IFNULL(external_task_id, '')) STORED,
    UNIQUE (source, external_task_id_not_null)
);

CREATE INDEX IF NOT EXISTS idx_task_status ON task (status);

CREATE INDEX IF NOT EXISTS idx_task_created_at ON task (created_at);

-- 事件流水：记录任务完整生命周期，用于审计、排障与离线补偿
CREATE TABLE IF NOT EXISTS task_event (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id INTEGER NOT NULL,
    event_type TEXT NOT NULL,
    raw_payload TEXT,
    created_at TEXT NOT NULL,
    CONSTRAINT fk_task_event_task FOREIGN KEY (task_id) REFERENCES task(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_task_event_task_id_created ON task_event (task_id, created_at, id);
