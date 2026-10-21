"""Convert ONNX with first-generation RKNN Toolkit in its separate environment."""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import host_env

os.environ.setdefault("TF_NUM_INTRAOP_THREADS", "2")
os.environ.setdefault("TF_NUM_INTEROP_THREADS", "2")
os.environ.setdefault("OMP_NUM_THREADS", "2")

ROOT = Path(__file__).resolve().parents[1]


def checked(result, operation):
    if result not in (None, 0):
        raise RuntimeError("{} failed: {}".format(operation, result))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--onnx", type=Path, default=ROOT / "artifacts/objects.onnx")
    parser.add_argument("--dataset", type=Path, default=ROOT / "artifacts/calibration/dataset.txt")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/objects-rv1126.rknn")
    parser.add_argument("--no-quantize", action="store_true")
    args = parser.parse_args()
    version = importlib.metadata.version("rknn-toolkit")
    if not version.startswith("1."):
        raise RuntimeError("RV1126 requires first-generation rknn-toolkit, not Toolkit2")
    if not args.onnx.is_file() or not args.onnx.with_suffix(".json").is_file():
        parser.error("Export a trained model and its metadata first")
    if not args.no_quantize:
        if not args.dataset.is_file():
            parser.error("Prepare calibration images first")
        entries = args.dataset.read_text().splitlines()
        if not entries or any(not Path(entry).is_file() for entry in entries):
            parser.error("Invalid calibration image list")
    from rknn.api import RKNN
    rknn = RKNN(verbose=True)
    try:
        checked(rknn.config(target_platform=["rv1126"], mean_values=[[0, 0, 0]],
                            std_values=[[255, 255, 255]], reorder_channel="0 1 2",
                            output_optimize=0), "config")
        checked(rknn.load_onnx(model=str(args.onnx.resolve())), "load_onnx")
        checked(rknn.build(do_quantization=not args.no_quantize,
                           dataset=None if args.no_quantize else str(args.dataset.resolve())), "build")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        checked(rknn.export_rknn(str(args.output)), "export_rknn")
        metadata = json.loads(args.onnx.with_suffix(".json").read_text())
        metadata.update(toolkit_version=version, quantized=not args.no_quantize,
                        device_validation="NOT YET RUN")
        args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2, ensure_ascii=False))
        print("Exported:", args.output, "Board runtime compatibility must still be tested.")
    finally:
        rknn.release()


if __name__ == "__main__":
    main()
