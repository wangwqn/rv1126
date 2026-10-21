"""Export trained ReLU weights into three static NCHW convolution heads."""
import argparse
import json
from pathlib import Path
import sys
import types
import host_env

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party/yolov5"))
import numpy as np
import onnx
import onnxruntime as ort
import torch


def raw_heads(self, features):
    return tuple(layer(feature) for layer, feature in zip(self.m, features))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--size", type=int, default=320)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/objects.onnx")
    args = parser.parse_args()
    if args.size < 32 or args.size % 32:
        parser.error("Size must be a positive multiple of 32")
    torch.set_num_threads(2)
    # Only load checkpoints produced locally or obtained from a trusted publisher.
    checkpoint = torch.load(str(args.weights), map_location="cpu")
    model = (checkpoint.get("ema") or checkpoint["model"]).float().eval()
    if any(isinstance(layer, torch.nn.SiLU) for layer in model.modules()):
        raise RuntimeError("SiLU checkpoint: train with scripts/train.py before exporting")
    classes = json.loads((ROOT / "config/classes.json").read_text())
    names = [item["name"] for item in classes]
    actual_names = model.names
    if isinstance(actual_names, dict):
        actual_names = [actual_names[i] for i in range(len(actual_names))]
    if list(actual_names) != names:
        raise RuntimeError("Checkpoint class order does not match config/classes.json")
    detector = model.model[-1]
    if detector.nl != 3:
        raise RuntimeError("Expected exactly three YOLO output heads")
    strides = detector.stride.detach().cpu().tolist()
    anchors = (detector.anchors * detector.stride.view(-1, 1, 1)).detach().cpu().tolist()
    detector.forward = types.MethodType(raw_heads, detector)
    sample = torch.rand(1, 3, args.size, args.size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        expected = [value.numpy() for value in model(sample)]
        torch.onnx.export(model, sample, str(args.output), opset_version=12,
                          input_names=["images"], output_names=["head8", "head16", "head32"],
                          do_constant_folding=True)
    onnx.checker.check_model(onnx.load(str(args.output)))
    session = ort.InferenceSession(str(args.output), providers=["CPUExecutionProvider"])
    actual = session.run(None, {"images": sample.numpy()})
    for reference, exported in zip(expected, actual):
        np.testing.assert_allclose(reference, exported, rtol=1e-3, atol=1e-4)
    metadata = {"schema": 1, "target": "rv1126", "size": args.size, "classes": classes,
                "input": "RGB uint8 NHWC on device; RGB float32 NCHW /255 in ONNX",
                "resize": "letterbox, centered, fill=114", "strides": strides,
                "anchors": anchors, "outputs": "raw logits NCHW; sigmoid/decode/NMS on CPU",
                "output_shapes": [list(value.shape) for value in actual],
                "training_weights": str(args.weights.resolve())}
    args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2, ensure_ascii=False))
    print("ONNX checker and PyTorch/ONNX Runtime numerical comparison passed:", args.output)


if __name__ == "__main__":
    main()
