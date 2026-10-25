"""生成 MiniMax Code 接入插件的图标 icon.png（纯标准库，无第三方依赖）。

图形：品牌橙圆角方块 + 白色收件箱托盘（AI Task Hub 的收件箱意象）。
用法：python scripts/gen_plugin_icon.py
"""

import struct
import zlib
from pathlib import Path

SIZE = 64
BG = (223, 118, 84, 255)      # --brand #df7654
FG = (255, 255, 255, 255)
RADIUS = 14
OUT = Path(__file__).resolve().parent.parent / "adapters" / "minimax-code" / "icon.png"


def rounded_square(x: int, y: int, size: int, radius: int) -> bool:
    """判断像素是否落在圆角方块内。"""
    left = top = radius
    right = bottom = size - 1 - radius
    cx = min(max(x, left), right)
    cy = min(max(y, top), bottom)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def in_tray(x: int, y: int) -> bool:
    """收件箱托盘：外框 + 顶部开口的挡板。"""
    if 14 <= x <= 49 and 16 <= y <= 19:
        return True
    if 14 <= x <= 49 and 30 <= y <= 45:
        return True
    if 14 <= x <= 19 and 16 <= y <= 45:
        return True
    if 44 <= x <= 49 and 16 <= y <= 45:
        return True
    # 中间上凸的投递口
    if 24 <= x <= 39 and 22 <= y <= 30:
        return True
    return False


def build_pixels() -> bytes:
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)  # filter type 0
        for x in range(SIZE):
            if not rounded_square(x, y, SIZE, RADIUS):
                rows.extend((0, 0, 0, 0))
            elif in_tray(x, y):
                rows.extend(FG)
            else:
                rows.extend(BG)
    return bytes(rows)


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def main() -> None:
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(build_pixels(), 9))
        + chunk(b"IEND", b"")
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(png)
    print(f"wrote {OUT} ({len(png)} bytes)")


if __name__ == "__main__":
    main()
