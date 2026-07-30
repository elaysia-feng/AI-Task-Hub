"""统一日志配置：滚动文件 + 控制台，开发/打包形态自动切换目录。

开发态写入仓库 logs/；PyInstaller 打包态写入 %APPDATA%/AI Task Hub/logs/
（exe 所在目录可能无写权限，且日志属于用户数据而非程序文件）。
"""

import logging
import os
import sys
from logging.handlers import RotatingFileHandler
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

    if not any(isinstance(h, RotatingFileHandler) for h in root.handlers):
        file_handler = RotatingFileHandler(
            path, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8"
        )
        file_handler.setFormatter(formatter)
        root.addHandler(file_handler)

    if not any(isinstance(h, logging.StreamHandler) and not isinstance(h, RotatingFileHandler) for h in root.handlers):
        console = logging.StreamHandler()
        console.setFormatter(formatter)
        root.addHandler(console)

    return path
