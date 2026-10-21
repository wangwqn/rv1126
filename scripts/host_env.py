"""Keep third-party caches inside this isolated project."""
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
for key, subdir in (("YOLOV5_CONFIG_DIR", "yolo"), ("MPLCONFIGDIR", "matplotlib"),
                    ("TORCH_HOME", "torch"), ("XDG_CACHE_HOME", "xdg")):
    path = ROOT / ".cache" / subdir
    path.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault(key, str(path))
# Upstream checks Arial at import. Use local DejaVu for English training plots,
# avoiding an implicit download; Chinese deployment labels use Noto CJK.
font = Path(os.environ["YOLOV5_CONFIG_DIR"]) / "Arial.ttf"
if not font.exists():
    shutil.copyfile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", font)
