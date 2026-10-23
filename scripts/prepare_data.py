"""Select real COCO images and remap their YOLO labels to our ordered classes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import random
import shutil
import urllib.request
import zipfile
import time

ROOT = Path(__file__).resolve().parents[1]
LABEL_URL = "https://github.com/ultralytics/yolov5/releases/download/v1.0/coco2017labels.zip"


def download(url, path):
    path = Path(path)
    if path.exists() and path.stat().st_size:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".part")
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "rv1126-training/1.0"})
            with urllib.request.urlopen(request, timeout=90) as response, temporary.open("wb") as target:
                shutil.copyfileobj(response, target)
            temporary.replace(path)
            return
        except Exception:
            if attempt == 2:
                raise
            time.sleep(1 + attempt)


def remap(text, mapping):
    labels = []
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 5:
            raise ValueError("Expected bounding-box labels, not segmentation labels")
        source = int(fields[0])
        if source in mapping:
            labels.append(str(mapping[source]) + " " + " ".join(fields[1:]))
    return labels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--classes", type=Path, default=ROOT / "config/classes.json")
    parser.add_argument("--train-per-class", type=int, default=60)
    parser.add_argument("--val-per-class", type=int, default=15)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--resume", action="store_true", help="Retry the same deterministic subset after a download failure")
    parser.add_argument("--output", type=Path, default=ROOT / "datasets/coco_objects")
    args = parser.parse_args()
    if min(args.train_per_class, args.val_per_class) <= 0:
        parser.error("Sample counts must be positive")
    if args.output.exists() and not args.resume:
        parser.error("Output exists; choose a new --output to avoid mixing dataset versions")
    classes = json.loads(args.classes.read_text())
    plan = {"classes": classes, "seed": args.seed, "train_per_class": args.train_per_class,
            "val_per_class": args.val_per_class}
    plan_path = args.output / "selection.json"
    if plan_path.exists() and json.loads(plan_path.read_text()) != plan:
        parser.error("Resume parameters differ from the original selection")
    args.output.mkdir(parents=True, exist_ok=True)
    plan_path.write_text(json.dumps(plan, indent=2, ensure_ascii=False))
    mapping = {item["coco_index"]: index for index, item in enumerate(classes)}
    archive = ROOT / "downloads/coco2017labels.zip"
    print("Downloading COCO bounding-box annotations...", flush=True)
    download(LABEL_URL, archive)
    rng = random.Random(args.seed)
    report = {"source": LABEL_URL, "seed": args.seed, "classes": classes, "splits": {}}
    with zipfile.ZipFile(archive) as source:
        for split, count in (("train", args.train_per_class), ("val", args.val_per_class)):
            candidates = {index: [] for index in range(len(classes))}
            records = {}
            for name in source.namelist():
                if "/labels/" + split + "2017/" not in name or not name.endswith(".txt"):
                    continue
                labels = remap(source.read(name).decode("utf-8"), mapping)
                if not labels:
                    continue
                image_id = Path(name).stem
                records[image_id] = labels
                for index in {int(line.split()[0]) for line in labels}:
                    candidates[index].append(image_id)
            selected = set()
            for index, ids in candidates.items():
                if not ids:
                    raise ValueError("No images for " + classes[index]["name"])
                ids.sort()
                rng.shuffle(ids)
                selected.update(ids[:count])
            jobs = []
            instances = [0] * len(classes)
            for image_id in sorted(selected):
                label = args.output / "labels" / split / (image_id + ".txt")
                label.parent.mkdir(parents=True, exist_ok=True)
                label.write_text("\n".join(records[image_id]) + "\n")
                for line in records[image_id]:
                    instances[int(line.split()[0])] += 1
                image = args.output / "images" / split / (image_id + ".jpg")
                # Path-style S3 HTTPS avoids the dotted bucket's TLS hostname mismatch.
                url = "https://s3.amazonaws.com/images.cocodataset.org/{}2017/{}.jpg".format(split, image_id)
                jobs.append((url, image))
            print("{}: downloading {} images".format(split, len(jobs)), flush=True)
            with ThreadPoolExecutor(max_workers=6) as pool:
                for n, _ in enumerate(pool.map(lambda job: download(*job), jobs), 1):
                    if n % 50 == 0:
                        print("{} {}/{}".format(split, n, len(jobs)), flush=True)
            report["splits"][split] = {"images": len(jobs), "instances_per_class": instances,
                                        "image_ids": sorted(selected)}
    if set(report["splits"]["train"]["image_ids"]) & set(report["splits"]["val"]["image_ids"]):
        raise ValueError("Training/validation image leakage")
    names = [item["name"] for item in classes]
    # JSON string syntax is valid YAML; no third-party dependency is needed here.
    (args.output / "data.yaml").write_text(
        "path: " + json.dumps(str(args.output.resolve())) + "\n"
        "train: images/train\nval: images/val\n"
        "nc: " + str(len(names)) + "\nnames: " + json.dumps(names) + "\n")
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print("Dataset ready:", args.output / "data.yaml", flush=True)


if __name__ == "__main__":
    main()
