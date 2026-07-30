"""AI Task Hub 本地事件服务入口。

桌面端（Electron）会自动拉起本服务；也可独立运行：
    python -m app.main
    uvicorn --factory app.api.app:create_app --host 127.0.0.1 --port 17891
"""

import sys
from pathlib import Path

# 兼容 python app/main.py 直接运行
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import uvicorn

from app.api.app import create_app
from app.logging_config import setup_logging
from shared.constants import API_HOST, API_PORT, APP_VERSION


def main() -> None:
    import logging

    log_path = setup_logging()
    logging.getLogger(__name__).info(
        "AI Task Hub 事件服务启动 v%s，监听 %s:%s，日志：%s",
        APP_VERSION, API_HOST, API_PORT, log_path,
    )
    # 直接传入工厂函数（静态可分析，PyInstaller 能跟随依赖）；
    # log_config=None：uvicorn 复用 root logger（滚动文件 + 控制台）
    uvicorn.run(
        create_app,
        host=API_HOST,
        port=API_PORT,
        log_level="info",
        factory=True,
        log_config=None,
    )


if __name__ == "__main__":
    main()
