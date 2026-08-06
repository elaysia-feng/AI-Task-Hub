from datetime import datetime


def now() -> datetime:
    """当前本地时间（naive，落库 DATETIME(3) 使用）。

    截断到毫秒：MySQL DATETIME(3) 只保留 3 位小数，SQLite 却按 TEXT 存完整微秒，
    直接 return datetime.now() 会让两后端读回精度不一致，比较/测试时出现亚毫秒假差异。
    """
    dt = datetime.now()
    return dt.replace(microsecond=(dt.microsecond // 1000) * 1000)


def to_local_naive(dt: datetime) -> datetime:
    """带时区的 datetime → 本地 naive（MySQL DATETIME 不存时区）。"""
    if dt.tzinfo is None:
        return dt
    return dt.astimezone().replace(tzinfo=None)
