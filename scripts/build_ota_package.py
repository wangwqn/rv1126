#!/usr/bin/env python3
"""Build and sign a complete RV1126 application OTA bundle."""

import argparse
import base64
import hashlib
import json
import os
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path


FILES = (
    ("VERSION", "VERSION", 0o644),
    ("objects/objects.rknn", "objects.rknn", 0o644),
    ("objects/objects.json", "objects.json", 0o644),
    ("objects/labels.png", "labels.png", 0o644),
    ("objects/object_image", "object_image", 0o755),
    ("objects/rv1126-object", "rv1126-object", 0o755),
    ("objects/rv1126-web", "rv1126-web", 0o755),
    ("web/rv1126_web.py", "rv1126-web.py", 0o755),
    ("web/web-index.html", "web-index.html", 0o644),
    ("web/web-app.js", "web-app.js", 0o644),
    ("web/web-styles.css", "web-styles.css", 0o644),
    ("objects/rkmedia_object_detection_rtsp", "rkmedia_object_detection_rtsp", 0o755),
)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description="Build a signed RV1126 .rvota bundle")
    parser.add_argument("--release", default="release/rv1126")
    parser.add_argument("--private-key", required=True)
    parser.add_argument("--trusted-key", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--sequence", type=int)
    parser.add_argument("--min-version", required=True)
    parser.add_argument("--product", default="rv1126-vision")
    parser.add_argument("--device-model", default="ATK-DLRV1126")
    parser.add_argument("--architecture", default="armv7l")
    parser.add_argument("--git-commit")
    args = parser.parse_args()

    release = Path(args.release).resolve()
    private_key = Path(args.private_key).resolve()
    trusted_key = json.loads(Path(args.trusted_key).read_text(encoding="utf-8"))
    output = Path(args.output).resolve()
    if not private_key.is_file():
        parser.error("private key does not exist")
    version = (release / "VERSION").read_text(encoding="utf-8").strip()
    parts = version.split(".")
    if len(parts) != 3 or not all(item.isdigit() for item in parts):
        parser.error("release VERSION must be major.minor.patch")
    default_sequence = int(parts[0]) * 1000000 + int(parts[1]) * 1000 + int(parts[2])
    sequence = args.sequence if args.sequence is not None else default_sequence
    if sequence < 1:
        parser.error("sequence must be positive")
    git_commit = args.git_commit
    if not git_commit:
        git_commit = subprocess.check_output(
            ["git", "-C", str(release.parents[1]), "rev-parse", "HEAD"], text=True).strip()

    with tempfile.TemporaryDirectory(prefix="rv1126-ota-build.") as temporary_name:
        temporary = Path(temporary_name)
        payload = temporary / "payload"
        payload.mkdir()
        entries = []
        for source_name, target_name, mode in FILES:
            source = release / source_name
            if not source.is_file():
                parser.error("release file is missing: " + source_name)
            target = payload / target_name
            target.write_bytes(source.read_bytes())
            os.chmod(target, mode)
            entries.append({
                "path": target_name,
                "sha256": digest(target),
                "size": target.stat().st_size,
                "mode": mode,
            })
        commit_file = payload / "GIT_COMMIT"
        commit_file.write_text(git_commit + "\n", encoding="utf-8")
        os.chmod(commit_file, 0o644)
        entries.append({"path": "GIT_COMMIT", "sha256": digest(commit_file),
                        "size": commit_file.stat().st_size, "mode": 0o644})
        entries.sort(key=lambda item: item["path"])
        manifest = {
            "schema": 1,
            "product": args.product,
            "device_model": args.device_model,
            "architecture": args.architecture,
            "version": version,
            "sequence": sequence,
            "min_version": args.min_version,
            "key_id": trusted_key["key_id"],
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "git_commit": git_commit,
            "files": entries,
        }
        manifest_path = temporary / "manifest.json"
        manifest_path.write_bytes(canonical(manifest))
        signature_path = temporary / "manifest.sig.raw"
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(private_key),
                        "-out", str(signature_path), str(manifest_path)], check=True)
        encoded_signature = base64.b64encode(signature_path.read_bytes())
        (temporary / "manifest.sig").write_bytes(encoded_signature)

        output.parent.mkdir(parents=True, exist_ok=True)
        partial = output.with_suffix(output.suffix + ".new")
        with tarfile.open(str(partial), "w:gz") as archive:
            archive.add(manifest_path, arcname="manifest.json", recursive=False)
            archive.add(temporary / "manifest.sig", arcname="manifest.sig", recursive=False)
            for entry in entries:
                archive.add(payload / entry["path"], arcname="payload/" + entry["path"],
                            recursive=False)
        os.replace(partial, output)
    print(json.dumps({"bundle": str(output), "version": version,
                      "sequence": sequence, "sha256": digest(output),
                      "key_id": trusted_key["key_id"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
