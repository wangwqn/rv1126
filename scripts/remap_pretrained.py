"""Transfer selected COCO detection-head channels as well as the backbone."""
import argparse
import json
from pathlib import Path
import sys
import host_env

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party/yolov5"))
import torch


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--weights", default=str(ROOT / "downloads/yolov5n-v6.0.pt"))
    parser.add_argument("--output", default=str(ROOT / "artifacts/init11.pt"))
    args = parser.parse_args()
    classes = json.loads((ROOT / "config/classes.json").read_text())
    checkpoint = torch.load(args.weights, map_location="cpu")  # trusted official checkpoint only
    model = checkpoint["model"].float()
    head = model.model[-1]
    assert head.nc == 80 and head.na == 3
    select = [0, 1, 2, 3, 4] + [5 + c["coco_index"] for c in classes]
    for index, old in enumerate(head.m):
        layer = torch.nn.Conv2d(old.in_channels, head.na * len(select), 1)
        with torch.no_grad():
            layer.weight.copy_(old.weight.reshape(3, 85, old.in_channels, 1, 1)[:, select].reshape_as(layer.weight))
            layer.bias.copy_(old.bias.reshape(3, 85)[:, select].reshape_as(layer.bias))
        head.m[index] = layer
    head.nc, head.no = len(classes), len(classes) + 5
    model.yaml["nc"] = len(classes)
    model.names = [c["name"] for c in classes]
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    torch.save({"model": model, "optimizer": None, "ema": None, "epoch": -1,
                "best_fitness": 0.0, "updates": 0}, output)
    print("Initialized selected COCO channels; this is NOT a newly trained model:", output)


if __name__ == "__main__":
    main()
