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

from shared.constants import API_HOST, API_PORT


def main() -> None:
    uvicorn.run(
        "app.api.app:create_app",
        host=API_HOST,
        port=API_PORT,
        log_level="info",
        factory=True,
    )


if __name__ == "__main__":
    main()
