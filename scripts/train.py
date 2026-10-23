"""Train a ReLU YOLOv5n with the pinned upstream training implementation."""
import os
from pathlib import Path
import sys
import warnings
import host_env

ROOT = Path(__file__).resolve().parents[1]
YOLO = ROOT / "third_party/yolov5"
os.environ.setdefault("OMP_NUM_THREADS", "2")
os.environ.setdefault("MKL_NUM_THREADS", "2")
os.environ.setdefault("WANDB_DISABLED", "true")
warnings.filterwarnings(
    "ignore",
    message="User provided device_type of 'cuda', but CUDA is not available.*",
)
sys.path.insert(0, str(YOLO))

import torch
from models.common import Conv

# Change the activation BEFORE creating/training a model, never only at export.
original_init = Conv.__init__


def relu_init(self, *args, **kwargs):
    original_init(self, *args, **kwargs)
    if isinstance(self.act, torch.nn.SiLU):
        self.act = torch.nn.ReLU(inplace=True)


Conv.__init__ = relu_init
torch.set_num_threads(2)
import train as upstream_train

# Validate our pinned, headless environment without upstream's automatic pip
# installation (which otherwise adds a second OpenCV distribution).
def check_environment(**_):
    import pkg_resources
    requirements = (ROOT / "requirements-train.txt").read_text().splitlines()
    pkg_resources.require([r for r in requirements if r.strip() and not r.startswith("#")])


upstream_train.check_requirements = check_environment
upstream_train.check_git_status = lambda: None  # no network or git mutation at training time


if __name__ == "__main__":
    options = upstream_train.parse_opt()
    if options.resume:
        raise SystemExit("Use --weights with a trusted local checkpoint for this ReLU training wrapper")
    # Always construct the pinned model, then transfer checkpoint tensors into it.
    options.cfg = str(YOLO / "models/yolov5n.yaml")
    if Path(options.weights).is_file():
        checkpoint = torch.load(options.weights, map_location="cpu")
        previous = checkpoint["model"]
        if not any(isinstance(m, torch.nn.SiLU) for m in previous.modules()):
            # Preserve learned anchors when fine-tuning our own ReLU checkpoint.
            options.cfg = ""
        del previous, checkpoint
    upstream_train.main(options)
