"""桌宠立绘绿幕抠图与预处理脚本。

将带有纯绿背景（#00FF00）的生成原图批量转换为透明背景的 RGBA PNG，
仅从图像四边连通区域抠除绿色背景，避免误伤角色身上的青绿色丝带等细节。

用法示例：
    python scripts/make_pet_sprites.py <输入目录> <输出目录> [--preview <预览图路径>]
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SPRITE_NAMES = ("idle", "idle_blink", "notice", "happy", "drag", "walk", "sit", "sleep")
TARGET_HEIGHT = 640
CROP_PADDING = 8
KEY_LOW = 20
KEY_HIGH = 60
KEY_CORE_SEED = 60


def flood_fill_green_background(mask: np.ndarray, core_seeds: np.ndarray) -> np.ndarray:
    """从四条边界与纯绿核心种子出发，找出连通的背景与抗锯齿边缘区域（4 连通）。

    既能清理发丝/手臂围住的封闭纯绿空洞，又不会误伤身上 key 较低的青绿色丝带。
    """
    height, width = mask.shape
    visited = np.zeros((height, width), dtype=bool)
    seed_mask = np.zeros((height, width), dtype=bool)
    seed_mask[0, :] = mask[0, :]
    seed_mask[-1, :] = mask[-1, :]
    seed_mask[:, 0] = mask[:, 0]
    seed_mask[:, -1] = mask[:, -1]
    seed_mask |= mask & core_seeds

    ys, xs = np.nonzero(seed_mask)
    queue: deque[tuple[int, int]] = deque()
    for y, x in zip(ys.tolist(), xs.tolist()):
        if not visited[y, x]:
            visited[y, x] = True
            queue.append((y, x))

    # 按行扫描左右扩展，大幅减少 Python 循环次数
    while queue:
        y, x = queue.popleft()
        left = x
        while left > 0 and mask[y, left - 1] and not visited[y, left - 1]:
            left -= 1
            visited[y, left] = True
        right = x
        while right + 1 < width and mask[y, right + 1] and not visited[y, right + 1]:
            right += 1
            visited[y, right] = True

        for ny in (y - 1, y + 1):
            if 0 <= ny < height:
                for nx in range(left, right + 1):
                    if mask[ny, nx] and not visited[ny, nx]:
                        visited[ny, nx] = True
                        queue.append((ny, nx))

    return visited


def process_sprite(input_path: Path, output_path: Path) -> Image.Image:
    """对单张绿幕图执行连通抠图、去绿溢色、裁剪留边与等比缩放。"""
    img = Image.open(input_path).convert("RGBA")
    arr = np.array(img, dtype=np.int16)
    r = arr[:, :, 0]
    g = arr[:, :, 1]
    b = arr[:, :, 2]

    max_rb = np.maximum(r, b)
    key = g - max_rb

    # 从图像四边和明显纯绿区域(key >= 100)连通抠图，避免误伤青绿色丝带
    bg_mask = flood_fill_green_background(key > KEY_LOW, key >= KEY_CORE_SEED)

    alpha = np.full(key.shape, 255, dtype=np.uint8)
    alpha[bg_mask & (key >= KEY_HIGH)] = 0

    edge_mask = bg_mask & (key > KEY_LOW) & (key < KEY_HIGH)
    if np.any(edge_mask):
        edge_alpha = np.clip(
            np.round((KEY_HIGH - key[edge_mask]) / float(KEY_HIGH - KEY_LOW) * 255.0),
            0,
            255,
        ).astype(np.uint8)
        alpha[edge_mask] = edge_alpha
        # 半透明边缘去绿溢色
        g[edge_mask] = np.minimum(g[edge_mask], max_rb[edge_mask])

    out = np.empty((arr.shape[0], arr.shape[1], 4), dtype=np.uint8)
    out[:, :, 0] = np.clip(r, 0, 255).astype(np.uint8)
    out[:, :, 1] = np.clip(g, 0, 255).astype(np.uint8)
    out[:, :, 2] = np.clip(b, 0, 255).astype(np.uint8)
    out[:, :, 3] = alpha

    # 将完全透明区域的 RGB 归零，防止缩放插值时绿底颜色渗入边缘
    out[alpha == 0, :3] = 0

    ys, xs = np.nonzero(alpha > 0)
    if len(xs) == 0 or len(ys) == 0:
        raise ValueError(f"抠图结果为空，请检查原图背景色: {input_path}")

    x0, x1 = int(xs.min()), int(xs.max()) + 1
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    cropped = Image.fromarray(out[y0:y1, x0:x1], mode="RGBA")

    padded = Image.new(
        "RGBA",
        (cropped.width + CROP_PADDING * 2, cropped.height + CROP_PADDING * 2),
        (0, 0, 0, 0),
    )
    padded.paste(cropped, (CROP_PADDING, CROP_PADDING))

    new_width = max(1, round(padded.width * TARGET_HEIGHT / padded.height))
    resized = padded.resize((new_width, TARGET_HEIGHT), Image.Resampling.LANCZOS)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    resized.save(output_path, format="PNG")
    return resized


def build_preview(sprites: dict[str, Image.Image], preview_path: Path) -> None:
    """将处理后的立绘分别叠加在深色(#111416)和浅色(#f4f2ed)背景上输出预览图。"""
    names = [name for name in SPRITE_NAMES if name in sprites]
    if not names:
        return

    cell_w = max(sprites[n].width for n in names) + 40
    cell_h = TARGET_HEIGHT + 60
    canvas = Image.new("RGB", (cell_w * len(names), cell_h * 2), (17, 20, 22))
    draw = ImageDraw.Draw(canvas)

    rows = [
        ("dark #111416", (17, 20, 22), (241, 240, 236)),
        ("light #f4f2ed", (244, 242, 237), (32, 38, 49)),
    ]

    for row_idx, (theme_label, bg_color, text_color) in enumerate(rows):
        y_top = row_idx * cell_h
        draw.rectangle(
            [0, y_top, canvas.width, y_top + cell_h],
            fill=bg_color,
        )
        for col_idx, name in enumerate(names):
            sprite = sprites[name]
            x_left = col_idx * cell_w
            paste_x = x_left + (cell_w - sprite.width) // 2
            paste_y = y_top + 40
            canvas.paste(sprite, (paste_x, paste_y), sprite)
            draw.text((x_left + 16, y_top + 12), f"{name} ({theme_label})", fill=text_color)

    preview_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(preview_path, format="PNG")


def main() -> int:
    parser = argparse.ArgumentParser(description="桌宠绿幕抠图与透明 PNG 预处理工具")
    parser.add_argument("input_dir", type=Path, help="包含原始绿幕立绘 PNG 的目录")
    parser.add_argument("output_dir", type=Path, help="输出透明立绘 PNG 的目录")
    parser.add_argument(
        "--preview",
        type=Path,
        default=None,
        help="可选：输出深浅双底对比预览图的路径",
    )
    args = parser.parse_args()

    processed: dict[str, Image.Image] = {}
    for name in SPRITE_NAMES:
        src = args.input_dir / f"{name}.png"
        if not src.exists():
            print(f"[跳过] 未找到输入文件: {src}")
            continue
        dst = args.output_dir / f"{name}.png"
        processed[name] = process_sprite(src, dst)
        print(f"[完成] {src} -> {dst} ({processed[name].width}x{processed[name].height})")

    if not processed:
        raise FileNotFoundError(f"在目录 {args.input_dir} 中未找到任何立绘文件 ({', '.join(SPRITE_NAMES)})")

    if args.preview is not None:
        build_preview(processed, args.preview)
        print(f"[预览] 已生成对比图: {args.preview}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
