"""Prepare calibration images with the same centered letterbox used on device."""
import argparse
from pathlib import Path
import random
import cv2

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", type=Path, default=ROOT / "datasets/coco_objects/images/train")
    parser.add_argument("--size", type=int, default=320)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/calibration")
    args = parser.parse_args()
    if args.size < 32 or args.size % 32 or args.count < 1:
        parser.error("Invalid calibration size/count")
    paths = sorted(args.images.glob("*.jpg"))
    if not paths:
        parser.error("No training images found")
    random.Random(42).shuffle(paths)
    args.output.mkdir(parents=True, exist_ok=True)
    prepared = []
    for path in paths[:args.count]:
        image = cv2.imread(str(path))
        if image is None:
            raise RuntimeError("Unreadable image: " + str(path))
        height, width = image.shape[:2]
        ratio = min(args.size / width, args.size / height)
        new_w, new_h = round(width * ratio), round(height * ratio)
        resized = cv2.resize(image, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
        left, top = (args.size - new_w) // 2, (args.size - new_h) // 2
        image = cv2.copyMakeBorder(resized, top, args.size - new_h - top,
                                  left, args.size - new_w - left,
                                  cv2.BORDER_CONSTANT, value=(114, 114, 114))
        target = args.output / (path.stem + ".png")
        if not cv2.imwrite(str(target), image):
            raise RuntimeError("Cannot write calibration image")
        if any(char.isspace() for char in str(target.resolve())):
            raise RuntimeError("RKNN calibration paths must not contain whitespace")
        prepared.append(str(target.resolve()))
    (args.output / "dataset.txt").write_text("\n".join(prepared) + "\n")
    print("Calibration images:", len(prepared), args.output / "dataset.txt")


if __name__ == "__main__":
    main()
