"""Run exported raw heads on CPU, draw real predictions and save JSON."""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def predict(session, metadata, bgr, threshold=.35):
    size = metadata["size"]
    height, width = bgr.shape[:2]
    gain = min(size / width, size / height)
    w, h = round(width * gain), round(height * gain)
    left, top = (size - w) // 2, (size - h) // 2
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    canvas[top:top+h, left:left+w] = cv2.resize(rgb, (w, h), interpolation=cv2.INTER_LINEAR)
    heads = session.run(None, {"images": canvas.transpose(2, 0, 1)[None].astype(np.float32) / 255})
    boxes = []
    nc = len(metadata["classes"])
    for index, raw in enumerate(heads):
        grid = size // int(metadata["strides"][index])
        raw = raw.reshape(3, nc + 5, grid, grid).transpose(0, 2, 3, 1)
        prob = 1 / (1 + np.exp(-np.clip(raw, -80, 80)))
        scores = prob[..., 4, None] * prob[..., 5:]
        classes = np.argmax(scores, axis=-1)
        confidence = np.max(scores, axis=-1)
        for a, y, x in np.argwhere(confidence >= threshold):
            p = prob[a, y, x]
            cx, cy = (p[:2] * 2 - .5 + [x, y]) * metadata["strides"][index]
            bw, bh = (p[2:4] * 2) ** 2 * metadata["anchors"][index][a]
            coords = [(cx-bw/2-left)*width/w, (cy-bh/2-top)*height/h,
                      (cx+bw/2-left)*width/w, (cy+bh/2-top)*height/h]
            x1, y1 = np.floor(coords[:2]).astype(int)
            x2, y2 = np.ceil(coords[2:]).astype(int)
            x1, x2 = np.clip([x1, x2], 0, width).tolist()
            y1, y2 = np.clip([y1, y2], 0, height).tolist()
            if x2 > x1 and y2 > y1:
                boxes.append(dict(class_id=int(classes[a, y, x]), score=float(confidence[a, y, x]), box=[x1, y1, x2, y2]))
    kept = []
    for b in sorted(boxes, key=lambda item: -item["score"]):
        suppress = False
        for a in kept:
            if a["class_id"] != b["class_id"]:
                continue
            x1, y1, x2, y2 = a["box"]
            u1, v1, u2, v2 = b["box"]
            intersection = max(0, min(x2, u2) - max(x1, u1)) * max(0, min(y2, v2) - max(y1, v1))
            union = (x2-x1)*(y2-y1) + (u2-u1)*(v2-v1) - intersection
            if intersection / union > .45:
                suppress = True
                break
        if not suppress:
            kept.append(b)
        if len(kept) >= 100:
            break
    return kept


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--onnx", type=Path, default=ROOT / "artifacts/objects.onnx")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/preview.jpg")
    parser.add_argument("--threshold", type=float, default=.35)
    args = parser.parse_args()
    metadata = json.loads(args.onnx.with_suffix(".json").read_text())
    bgr = cv2.imread(str(args.image))
    if bgr is None:
        parser.error("Cannot read image")
    opts = ort.SessionOptions(); opts.intra_op_num_threads = 2
    session = ort.InferenceSession(str(args.onnx), opts, providers=["CPUExecutionProvider"])
    boxes = predict(session, metadata, bgr, args.threshold)
    image = Image.fromarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", size=20, index=2)
    for b in boxes:
        name = metadata["classes"][b["class_id"]]["zh"]
        b["name"] = name
        x1, y1, x2, y2 = b["box"]
        draw.rectangle((x1, y1, x2, y2), outline=(0, 255, 0), width=2)
        label = "{} {:.0%}".format(name, b["score"])
        panel_w = int(draw.textlength(label, font=font)) + 12
        x = max(0, min(x1, image.width - panel_w)); y = max(0, min(y1 - 30, image.height - 30))
        draw.rectangle((x, y, x + panel_w, y + 30), fill=(35, 35, 35))
        draw.text((x + 5, y + 3), label, fill="white", font=font, anchor="lt")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    args.output.with_suffix(".json").write_text(json.dumps(boxes, ensure_ascii=False, indent=2))
    print(json.dumps(boxes, ensure_ascii=False))


if __name__ == "__main__":
    main()
