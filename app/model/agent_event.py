from datetime import datetime
from enum import Enum
from typing import Optional

from pydantic import BaseModel, ConfigDict
from pydantic.alias_generators import to_camel


class TaskSource(str, Enum):
    CHATGPT = "CHATGPT"
    CLAUDE_CODE = "CLAUDE_CODE"
    CODEX = "CODEX"
    OTHER = "OTHER"


class EventType(str, Enum):
    TASK_STARTED = "TASK_STARTED"
    TASK_NEEDS_INPUT = "TASK_NEEDS_INPUT"
    TASK_COMPLETED = "TASK_COMPLETED"
    TASK_FAILED = "TASK_FAILED"
    TASK_VIEWED = "TASK_VIEWED"
    TASK_IGNORED = "TASK_IGNORED"


class AgentEvent(BaseModel):
    """统一事件模型，与 shared/event_schema.json 一一对应。

    线上传输使用 camelCase，Python 内部使用 snake_case。
    """

    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        use_enum_values=True,
    )

    source: TaskSource
    event_type: EventType
    external_task_id: Optional[str] = None
    title: Optional[str] = None
    content_preview: Optional[str] = None
    project_path: Optional[str] = None
    open_target: Optional[str] = None
    open_url: Optional[str] = None
    created_at: Optional[datetime] = None
