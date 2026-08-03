"""编码防回归：源码文件不得出现 GBK 破坏中文后留下的连续问号乱码。

本仓库多次发生中文注释被非 UTF-8 工具重写为连续问号的事故，
此测试在 CI 上兜底，一旦再次污染直接红（自身豁免扫描）。
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# glob patterns for auto-discovery (no manual directory list needed)
SCAN_PATTERNS = [
    "app/**/*.py", "app/**/*.ts", "app/**/*.js", "app/**/*.json",
    "tests/**/*.py", "tests/**/*.ts", "tests/**/*.js",
    "adapters/**/*.py", "adapters/**/*.ts", "adapters/**/*.js",
    "shared/**/*.py", "shared/**/*.ts", "shared/**/*.js",
    "scripts/**/*.py", "scripts/**/*.ts", "scripts/**/*.js",
    "desktop/src/**/*.ts", "desktop/src/**/*.js",
]
SKIP_NAMES = {"session_titles.json", "forward_target.json"}
MOJIBAKE_MARK = chr(63) * 3  # 连续三个问号：GBK 破坏中文后的典型残留
SELF = Path(__file__).resolve()


def test_no_mojibake_in_sources():
    offenders: list[str] = []
    for pattern in SCAN_PATTERNS:
        for path in REPO_ROOT.glob(pattern):
            # glob 模式已限定后缀，这里只做防御校验（no .md glob，故不放 .md）
            if path.suffix not in {".py", ".ts", ".js", ".json"} or not path.is_file():
                continue
            if path.name in SKIP_NAMES or path.resolve() == SELF:
                continue
            text = path.read_text(encoding="utf-8")
            if MOJIBAKE_MARK in text:
                offenders.append(str(path.relative_to(REPO_ROOT)))
    assert not offenders, f"发现 GBK 乱码污染的文件: {offenders}"
