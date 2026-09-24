#!/usr/bin/env python3
"""Build/test/stage isolated media-input diagnostics atop the standard prepared tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
MANIFEST = json.loads((ROOT / "manifest.json").read_text())


def run(args, cwd=None, **kwargs):
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True, **kwargs)


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["prepare", "build", "test", "stage"])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--baseline-runtime", type=Path)
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work = args.work_dir.resolve()
    chromium = work / "work/chromium/src"
    cef = chromium / "cef"
    base = json.loads((work / "prepared.json").read_text())
    if base["revision"] != MANIFEST["base_source_build_revision"]:
        raise RuntimeError("Prepare the pinned baseline runtime first")
    for repo, directory in [("cef", cef), ("chromium", chromium)]:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=directory, text=True).strip()
        if revision != MANIFEST[f"{repo}_revision"]:
            raise RuntimeError(f"Unexpected {repo} revision")
    for patch in MANIFEST["patches"]:
        path = ROOT / patch["file"]
        directory = cef if patch["repository"] == "cef" else chromium
        if sha(path) != patch["sha256"]:
            raise RuntimeError(f"Patch digest mismatch: {path}")
        applied = subprocess.run(["git", "apply", "--reverse", "--check", str(path)], cwd=directory,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not applied:
            if args.stage != "prepare":
                raise RuntimeError("Run prepare before build/test/stage")
            run(["git", "apply", "--check", path], cwd=directory)
            run(["git", "apply", path], cwd=directory)
    if args.stage == "prepare":
        return
    output = chromium / "out/Release_GN_x64"
    if args.stage in ("build", "test"):
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:args.jobs])
        target = "libcef" if args.stage == "build" else "cef_external_video_source_unittests"
        run([work / "depot_tools/ninja", "-C", output, f"-j{args.jobs}", target],
            env=dict(os.environ, DEPOT_TOOLS_UPDATE="0"))
        if args.stage == "test":
            run([output / target, "--gtest_filter=ExternalVideoSourceTest.*", "--test-launcher-jobs=1"], cwd=output)
        return
    if args.baseline_runtime is None or args.destination is None:
        parser.error("stage requires --baseline-runtime and --destination")
    destination = args.destination.resolve()
    if destination.exists():
        raise RuntimeError("Stage destination must be new")
    runtime = destination / "runtime"
    shutil.copytree(args.baseline_runtime.resolve(), runtime,
                    ignore=shutil.ignore_patterns("libcef.so", "miximus-source-build.json", ".staged"))
    shutil.copy2(output / "libcef.so", runtime / "libcef.so")
    # Blink/controller changes can regenerate the V8 context snapshot. Keep the
    # staged data paired with this build rather than the baseline shared library.
    for path in runtime.iterdir():
        built = output / path.name
        if path.suffix in (".bin", ".pak", ".dat") and built.is_file():
            shutil.copy2(built, path)
    link = destination / "link"
    link.mkdir()
    # Main-process loader search exposes libcef and its data, never ANGLE/Vulkan.
    for path in runtime.iterdir():
        if path.name == "libcef.so" or path.is_dir() or path.suffix in (".pak", ".bin", ".dat"):
            (link / path.name).symlink_to(Path("../runtime") / path.name)
    provenance = dict(MANIFEST, libcef_sha256=sha(runtime / "libcef.so"), experimental=True)
    (runtime / "miximus-media-input-prototype.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
