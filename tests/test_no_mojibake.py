"""编码防回归：源码文件不得出现 GBK 破坏中文后留下的连续问号乱码。

本仓库多次发生中文注释被非 UTF-8 工具重写为连续问号的事故，
此测试在 CI 上兜底，一旦再次污染直接红（自身豁免扫描）。
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["app", "tests", "adapters", "shared", "scripts"]
SCAN_SUFFIXES = {".py", ".ts", ".js", ".json", ".md"}
# 运行时缓存/日志（已在 .gitignore），不计入源码污染
SKIP_NAMES = {"session_titles.json", "forward_target.json", "notify_debug.log"}
MOJIBAKE_MARK = chr(63) * 3  # 连续三个问号：GBK 破坏中文后的典型残留
SELF = Path(__file__).resolve()


def test_no_mojibake_in_sources():
    offenders: list[str] = []
    for scan_dir in SCAN_DIRS:
        base = REPO_ROOT / scan_dir
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SCAN_SUFFIXES or not path.is_file():
                continue
            if path.name in SKIP_NAMES or path.resolve() == SELF:
                continue
            text = path.read_text(encoding="utf-8")
            if MOJIBAKE_MARK in text:
                offenders.append(str(path.relative_to(REPO_ROOT)))
    assert not offenders, f"发现 GBK 乱码污染的文件: {offenders}"
