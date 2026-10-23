"""Pre-render UTF-8 Chinese labels; the board needs no font library/file."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--classes", type=Path, default=ROOT / "config/classes.json")
    parser.add_argument("--font", default="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/labels.png")
    args = parser.parse_args()
    classes = json.loads(args.classes.read_text())
    font = ImageFont.truetype(args.font, size=22, index=2)  # Noto Sans CJK SC
    atlas = Image.new("RGB", (224, 32 * len(classes)), (35, 35, 35))
    draw = ImageDraw.Draw(atlas)
    for i, item in enumerate(classes):
        if draw.textlength(item["zh"], font=font) > 212:
            raise ValueError("Label exceeds atlas row: " + item["zh"])
        draw.text((6, i * 32 + 3), item["zh"], fill="white", font=font, anchor="lt")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(args.output)
    args.output.with_suffix(".json").write_text(json.dumps(classes, ensure_ascii=False, indent=2))
    print(args.output)


if __name__ == "__main__":
    main()
