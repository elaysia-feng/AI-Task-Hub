-- AI Task Hub MySQL Schema（InnoDB / utf8mb4）
-- 主键：BIGINT AUTO_INCREMENT；时间字段：DATETIME(3)（毫秒精度，存本地时间）
-- (source, external_task_id_not_null) 唯一约束用于跨平台事件幂等去重；NULL/空串经
-- generated column 归一到 ''，同源 NULL/空串事件合并去重到同一条任务（非「每次新建」）

CREATE TABLE IF NOT EXISTS task (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    source VARCHAR(32) NOT NULL COMMENT '事件来源平台：CHATGPT / CLAUDE_CODE / CODEX / OTHER',
    external_task_id VARCHAR(128) NULL COMMENT '平台侧任务/会话 ID，用于幂等去重',
    event_type VARCHAR(32) NOT NULL COMMENT '最近事件类型：TASK_STARTED / TASK_NEEDS_INPUT / TASK_COMPLETED / TASK_FAILED / TASK_VIEWED / TASK_IGNORED',
    title VARCHAR(512) NULL,
    content_preview TEXT NULL COMMENT '任务内容摘要（等待输入的问题、错误信息等）',
    project_path VARCHAR(1024) NULL COMMENT '项目目录，用于打开终端恢复会话',
    open_target VARCHAR(16) NULL COMMENT '打开方式：browser / terminal / none',
    open_url VARCHAR(2048) NULL COMMENT '浏览器打开地址（如 ChatGPT 对话链接）',
    status VARCHAR(32) NOT NULL COMMENT 'RUNNING / NEEDS_INPUT / COMPLETED_UNREAD / FAILED_UNREAD / VIEWED / IGNORED',
    created_at DATETIME(3) NOT NULL,
    completed_at DATETIME(3) NULL,
    viewed_at DATETIME(3) NULL,
    external_task_id_not_null VARCHAR(128) GENERATED ALWAYS AS (IFNULL(external_task_id, '')) STORED COMMENT '用于唯一约束占位，NULL 转为空字符串',
    UNIQUE KEY uk_source_external (source, external_task_id_not_null),
    KEY idx_status (status),
    KEY idx_created_at (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='统一任务表';

-- 事件流水：记录任务完整生命周期，用于审计、排障与离线补偿
CREATE TABLE IF NOT EXISTS task_event (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    task_id BIGINT NOT NULL,
    event_type VARCHAR(32) NOT NULL,
    raw_payload JSON NULL COMMENT '统一事件原始报文（AgentEvent JSON）',
    created_at DATETIME(3) NOT NULL,
    KEY idx_task_id_created (task_id, created_at, id),
    CONSTRAINT fk_task_event_task FOREIGN KEY (task_id) REFERENCES task(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='任务生命周期事件表';
