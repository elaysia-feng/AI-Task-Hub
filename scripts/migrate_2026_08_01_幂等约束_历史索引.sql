-- =============================================================================
-- AI-Task-Hub 数据库迁移脚本（幂等版）
-- 迁移日期：2026-08-01
-- 适用范围：本地 MySQL 8.0+（ai_task_hub / ai_task_hub_test）
--
-- 迁移目的：修复 worker-2 审计发现的两类历史问题
--   1. 幂等失效：MySQL UNIQUE 允许多个 NULL external_task_id，导致同源事件
--      在缺少平台侧会话 ID 时可被重复入库。修复方案为引入 generated column
--      把 NULL 折叠为空串，让唯一约束真正生效。
--   2. 历史查询慢：task_event 表只有单列 idx_task_id 索引，
--      按任务查时间线会走 filesort。改为 (task_id, created_at, id) 复合索引
--      后，排序与过滤均可走索引。
--
-- 幂等性：本脚本通过 INFORMATION_SCHEMA 判断对象是否存在后再决定操作，
-- 已迁移过的库再执行不会报错，可重复运行。
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 一、修复 task 表的幂等约束
-- -----------------------------------------------------------------------------

-- 1.1 删除旧的 (source, external_task_id) 唯一索引（若存在）
-- 说明：原索引对 NULL 不做去重（MySQL UNIQUE 允许多个 NULL），导致幂等失效。
SET @drop_old_uk := (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA = DATABASE()
              AND TABLE_NAME = 'task'
              AND INDEX_NAME = 'uk_source_external'
              AND COLUMN_NAME = 'external_task_id'
        ),
        'ALTER TABLE task DROP INDEX uk_source_external',
        'SELECT 1'
    )
);
PREPARE stmt FROM @drop_old_uk;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- 1.2 新增 generated column：把 NULL external_task_id 折叠为空串（若不存在）
-- 说明：
--   * 使用 STORED 而非 VIRTUAL 是为了让它能被索引（InnoDB 索引不能直接索引 VIRTUAL）。
--   * 计算逻辑 IFNULL(external_task_id, '') 在所有现有行上都成立（空串安全），
--     所以可以在线 ADD COLUMN，不需要重建表。
SET @add_col := (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
            WHERE TABLE_SCHEMA = DATABASE()
              AND TABLE_NAME = 'task'
              AND COLUMN_NAME = 'external_task_id_not_null'
        ),
        'SELECT 1',
        'ALTER TABLE task ADD COLUMN external_task_id_not_null VARCHAR(128) GENERATED ALWAYS AS (IFNULL(external_task_id, '''')) STORED COMMENT ''用于唯一约束占位：NULL 折叠为空串'''
    )
);
PREPARE stmt FROM @add_col;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- 1.3 重建唯一索引，作用于新的占位列（若不存在）
-- 说明：现在 (source, '') 也只允许一行，彻底堵住 NULL 绕过幂等的口子。
SET @add_new_uk := (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA = DATABASE()
              AND TABLE_NAME = 'task'
              AND INDEX_NAME = 'uk_source_external'
              AND COLUMN_NAME = 'external_task_id_not_null'
        ),
        'SELECT 1',
        'ALTER TABLE task ADD UNIQUE KEY uk_source_external (source, external_task_id_not_null)'
    )
);
PREPARE stmt FROM @add_new_uk;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- -----------------------------------------------------------------------------
-- 二、修复 task_event 表的历史查询索引
-- -----------------------------------------------------------------------------
-- 顺序很关键：必须先建新复合索引，再删旧单列索引。
-- 因为外键 fk_task_event_task 依赖 task_id 上的索引，
-- 新索引 (task_id, created_at, id) 以 task_id 为最左前缀，可以接替 FK 的索引需求。

-- 2.1 新增 (task_id, created_at, id) 复合索引（若不存在）
-- 说明：
--   * WHERE task_id = ? 仍可命中该索引前缀，覆盖单任务事件列表查询。
--   * WHERE task_id = ? ORDER BY created_at, id 直接走索引顺序，
--     消除 filesort，性能从 O(N log N) 降到 O(log N + 分页大小)。
SET @add_new_idx := (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA = DATABASE()
              AND TABLE_NAME = 'task_event'
              AND INDEX_NAME = 'idx_task_id_created'
        ),
        'SELECT 1',
        'ALTER TABLE task_event ADD INDEX idx_task_id_created (task_id, created_at, id)'
    )
);
PREPARE stmt FROM @add_new_idx;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- 2.2 删除旧的单列索引 idx_task_id（若存在）
-- 此时新复合索引已生效，外键约束会自动切换到新索引上，再删旧的就不会报错。
SET @drop_old_idx := (
    SELECT IF(
        EXISTS(
            SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA = DATABASE()
              AND TABLE_NAME = 'task_event'
              AND INDEX_NAME = 'idx_task_id'
        ),
        'ALTER TABLE task_event DROP INDEX idx_task_id',
        'SELECT 1'
    )
);
PREPARE stmt FROM @drop_old_idx;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- -----------------------------------------------------------------------------
-- 三、回滚脚本（仅作记录，请勿随迁移一起执行）
-- -----------------------------------------------------------------------------
-- ALTER TABLE task_event DROP INDEX idx_task_id_created;
-- ALTER TABLE task_event ADD INDEX idx_task_id (task_id);
--
-- ALTER TABLE task DROP INDEX uk_source_external;
-- ALTER TABLE task DROP COLUMN external_task_id_not_null;
-- ALTER TABLE task ADD UNIQUE KEY uk_source_external (source, external_task_id);
-- =============================================================================
