"""pytest 全局配置：把仓库根目录加入模块搜索路径，供适配器与编码检查测试导入。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
