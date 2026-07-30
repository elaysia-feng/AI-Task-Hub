from datetime import datetime


def now() -> datetime:
    """当前本地时间（naive，落库 DATETIME(3) 使用）。"""
    return datetime.now()


def to_local_naive(dt: datetime) -> datetime:
    """带时区的 datetime → 本地 naive（MySQL DATETIME 不存时区）。"""
    if dt.tzinfo is None:
        return dt
    return dt.astimezone().replace(tzinfo=None)
