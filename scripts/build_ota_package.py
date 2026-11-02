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
    ("VERSION", "VERSION", 0o644, "full"),
    ("objects/objects.rknn", "objects.rknn", 0o644, "model"),
    ("objects/objects.json", "objects.json", 0o644, "model"),
    ("objects/labels.png", "labels.png", 0o644, "model"),
    ("objects/object_image", "object_image", 0o755, "application"),
    ("objects/rv1126-object", "rv1126-object", 0o755, "application"),
    ("objects/rv1126-web", "rv1126-web", 0o755, "web"),
    ("web/rv1126_web.py", "rv1126-web.py", 0o755, "web"),
    ("web/web-index.html", "web-index.html", 0o644, "web"),
    ("web/web-app.js", "web-app.js", 0o644, "web"),
    ("web/web-styles.css", "web-styles.css", 0o644, "web"),
    ("objects/rkmedia_object_detection_rtsp", "rkmedia_object_detection_rtsp", 0o755, "application"),
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
    parser.add_argument("--channel", choices=("stable", "beta", "development"),
                        default="stable")
    parser.add_argument("--component", choices=("full", "application", "model", "web"),
                        default="full")
    parser.add_argument("--base-release",
                        help="build a file-level overlay against this release")
    parser.add_argument("--dependency", action="append", default=[], metavar="NAME=MIN_VERSION")
    parser.add_argument("--remove", action="append", default=[], metavar="RELEASE_PATH")
    parser.add_argument("--published-at")
    args = parser.parse_args()

    release = Path(args.release).resolve()
    private_key = Path(args.private_key).resolve()
    trusted_key = json.loads(Path(args.trusted_key).read_text(encoding="utf-8"))
    output = Path(args.output).resolve()
    base_release = Path(args.base_release).resolve() if args.base_release else None
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
    if args.component != "full" and base_release is None:
        parser.error("component packages require --base-release")
    base_version = None
    if base_release:
        if not base_release.is_dir() or not (base_release / "VERSION").is_file():
            parser.error("base release is missing VERSION")
        base_version = (base_release / "VERSION").read_text(encoding="utf-8").strip()
    dependencies = {"ota-runtime": "2.0.0"}
    for item in args.dependency:
        if "=" not in item:
            parser.error("dependency must use NAME=MIN_VERSION")
        name, minimum = item.split("=", 1)
        if not name or not minimum:
            parser.error("dependency must use NAME=MIN_VERSION")
        dependencies[name] = minimum
    published_at = args.published_at or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    with tempfile.TemporaryDirectory(prefix="rv1126-ota-build.") as temporary_name:
        temporary = Path(temporary_name)
        payload = temporary / "payload"
        payload.mkdir()
        entries = []
        for source_name, target_name, mode, component in FILES:
            if args.component != "full" and component != args.component:
                continue
            source = release / source_name
            if not source.is_file():
                parser.error("release file is missing: " + source_name)
            if base_release:
                base_source = base_release / source_name
                if not base_source.is_file():
                    base_source = base_release / target_name
                if base_source.is_file() and digest(base_source) == digest(source):
                    continue
            target = payload / target_name
            target.write_bytes(source.read_bytes())
            os.chmod(target, mode)
            entries.append({
                "path": target_name,
                "sha256": digest(target),
                "size": target.stat().st_size,
                "mode": mode,
            })
        if args.component in ("full", "application"):
            commit_file = payload / "GIT_COMMIT"
            commit_file.write_text(git_commit + "\n", encoding="utf-8")
            os.chmod(commit_file, 0o644)
            base_commit = base_release / "GIT_COMMIT" if base_release else None
            if not base_commit or not base_commit.is_file() or digest(base_commit) != digest(commit_file):
                entries.append({"path": "GIT_COMMIT", "sha256": digest(commit_file),
                                "size": commit_file.stat().st_size, "mode": 0o644})
        if not entries:
            parser.error("OTA overlay contains no changed files")
        entries.sort(key=lambda item: item["path"])
        manifest = {
            "schema": 2,
            "product": args.product,
            "device_model": args.device_model,
            "architecture": args.architecture,
            "version": version,
            "sequence": sequence,
            "min_version": args.min_version,
            "key_id": trusted_key["key_id"],
            "created_at": published_at,
            "published_at": published_at,
            "git_commit": git_commit,
            "channel": args.channel,
            "component": args.component,
            "package_type": "overlay" if base_release or args.component != "full" else "full",
            "base_version": base_version,
            "dependencies": dependencies,
            "remove": sorted(set(args.remove)),
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
        # RV1126 Buildroot Python has no zlib module. The payload is already
        # compact (quantized RKNN/binaries), so use an uncompressed signed tar.
        with tarfile.open(str(partial), "w:") as archive:
            archive.add(manifest_path, arcname="manifest.json", recursive=False)
            archive.add(temporary / "manifest.sig", arcname="manifest.sig", recursive=False)
            for entry in entries:
                archive.add(payload / entry["path"], arcname="payload/" + entry["path"],
                            recursive=False)
        os.replace(partial, output)
    print(json.dumps({"bundle": str(output), "version": version,
                      "sequence": sequence, "sha256": digest(output),
                      "key_id": trusted_key["key_id"], "channel": args.channel,
                      "component": args.component,
                      "package_type": manifest["package_type"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
