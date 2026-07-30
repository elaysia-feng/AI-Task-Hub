from datetime import datetime
from enum import Enum
from typing import Optional

from pydantic import BaseModel, ConfigDict
from pydantic.alias_generators import to_camel


class TaskStatus(str, Enum):
    RUNNING = "RUNNING"
    NEEDS_INPUT = "NEEDS_INPUT"
    COMPLETED_UNREAD = "COMPLETED_UNREAD"
    FAILED_UNREAD = "FAILED_UNREAD"
    VIEWED = "VIEWED"
    IGNORED = "IGNORED"


class Task(BaseModel):
    """任务模型。API 输出 camelCase，内部构造使用 snake_case。

    时间字段为本地 naive datetime，落库为 DATETIME(3)，API 序列化为 ISO 8601。
    """

    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
    )

    id: int  # 由 MySQL 自增分配，创建时以 0 占位
    source: str
    external_task_id: Optional[str] = None
    event_type: str
    title: Optional[str] = None
    content_preview: Optional[str] = None
    project_path: Optional[str] = None
    open_target: Optional[str] = None
    open_url: Optional[str] = None
    status: str
    created_at: datetime
    completed_at: Optional[datetime] = None
    viewed_at: Optional[datetime] = None
