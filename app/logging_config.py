"""统一日志配置：按日期滚动文件 + 控制台，开发/打包形态自动切换目录。

开发态写入仓库 logs/；PyInstaller 打包态写入 %APPDATA%/AI Task Hub/logs/
（exe 所在目录可能无写权限，且日志属于用户数据而非程序文件）。
日志按天分文件：今天的日志写 backend.log，跨天自动滚动为 backend.log.YYYY-MM-DD。
"""

import logging
import os
import sys
from logging.handlers import BaseRotatingHandler, TimedRotatingFileHandler
from pathlib import Path


def log_dir() -> Path:
    if getattr(sys, "frozen", False):
        base = Path(os.environ.get("APPDATA", str(Path.home()))) / "AI Task Hub"
    else:
        base = Path(__file__).resolve().parent.parent
    path = base / "logs"
    path.mkdir(parents=True, exist_ok=True)
    return path


def log_file() -> Path:
    return log_dir() / "backend.log"


def setup_logging(level: int = logging.INFO) -> Path:
    """挂载滚动文件与控制台处理器（幂等），返回日志文件路径。"""
    path = log_file()
    root = logging.getLogger()
    root.setLevel(level)
    formatter = logging.Formatter("%(asctime)s %(levelname)s [%(name)s] %(message)s")

    if not any(isinstance(h, BaseRotatingHandler) for h in root.handlers):
        # 按天滚动：今天的日志写在 backend.log，跨天滚动成 backend.log.YYYY-MM-DD；
        # backupCount=30 保留最近 30 天的历史日志，更早的自动清理。
        file_handler = TimedRotatingFileHandler(
            path, when="midnight", backupCount=30, encoding="utf-8"
        )
        # Windows 上 os.rename 在目标已存在/被短暂锁定时抛 PermissionError 导致轮转失败；
        # 改用 os.replace（原子覆盖），轮转更稳健
        file_handler.rotator = lambda source, dest: os.replace(source, dest)
        file_handler.setFormatter(formatter)
        root.addHandler(file_handler)

    if not any(isinstance(h, logging.StreamHandler) and not isinstance(h, BaseRotatingHandler) for h in root.handlers):
        # Windows 中文系统 stderr 默认 GBK，但日志内容已是 UTF-8，强制 stdout/stderr 走 UTF-8
        if sys.platform == "win32":
            for stream in (sys.stdout, sys.stderr):
                try:
                    stream.reconfigure(encoding="utf-8")
                except Exception as exc:
                    import logging as _logging
                    _logging.getLogger(__name__).warning("stream.reconfigure 失败: %s", exc)
        console = logging.StreamHandler()
        console.setFormatter(formatter)
        root.addHandler(console)

    return path
