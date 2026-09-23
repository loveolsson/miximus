#!/usr/bin/env python3
"""Explicit, resumable Linux CEF SDK build. Never called by application CMake."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request


ROOT = Path(__file__).resolve().parent
MANIFEST = json.loads((ROOT / "source-build.json").read_text())


def run(args, cwd, env=None):
    print("Running:", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_revision(path, revision):
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=path, text=True
    ).strip()
    if actual != revision:
        raise RuntimeError(f"{path}: expected revision {revision}, got {actual}")


def apply_once(path, patch, strip=1):
    # Require a repository root: git apply otherwise silently skips paths.
    root = subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], cwd=path, text=True
    ).strip()
    if Path(root).resolve() != path.resolve():
        raise RuntimeError(f"Not a repository root: {path}")
    args = ["git", "apply", f"-p{strip}"]
    already_applied = subprocess.run(
        args + ["--reverse", "--check", str(patch)], cwd=path,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if not already_applied:
        run(args + ["--check", patch], path)
        run(args + [patch], path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["sync", "prepare", "build", "test", "package"])
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if sys.platform != "linux" or args.jobs < 1:
        parser.error("Requires Linux and a positive job count")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    depot = work / "depot_tools"
    download = work / "work"
    chromium = download / "chromium/src"
    cef = chromium / "cef"
    env = dict(os.environ)
    env.update(DEPOT_TOOLS_UPDATE="0", GN_DEFINES=MANIFEST["gn_defines"])
    env["PATH"] = str(depot) + os.pathsep + env["PATH"]
    for patch in MANIFEST["patches"] + MANIFEST.get("test_patches", []):
        if digest(ROOT / patch["file"]) != patch["sha256"]:
            raise RuntimeError(f"Patch digest mismatch: {patch['file']}")

    if args.stage == "sync":
        # A source sync may revert Chromium changes. Never run it on our prepared tree.
        if (work / "prepared.json").exists() or (work / "preparing.json").exists():
            raise RuntimeError("Already prepared; resume build instead of syncing patched sources")
        if not depot.exists():
            run(["git", "clone", "https://chromium.googlesource.com/chromium/tools/depot_tools.git", depot], work)
            run(["git", "checkout", "--detach", MANIFEST["depot_tools_revision"]], depot)
        verify_revision(depot, MANIFEST["depot_tools_revision"])
        run([depot / "ensure_bootstrap"], work, env)
        automate = work / "automate-git.py"
        if not automate.exists():
            url = ("https://raw.githubusercontent.com/chromiumembedded/cef/"
                   + MANIFEST["cef_revision"] + "/tools/automate/automate-git.py")
            automate.write_bytes(urllib.request.urlopen(url, timeout=60).read())
        if digest(automate) != MANIFEST["automate_sha256"]:
            raise RuntimeError("CEF automation script digest mismatch")
        # Create the configuration before automation can substitute 'latest'
        # for siso or omit the PGO profile required by an official build.
        chromium_dir = download / "chromium"
        chromium_dir.mkdir(parents=True, exist_ok=True)
        version = json.loads((ROOT / "sdk.json").read_text())["chromium_version"]
        solutions = [{
            "managed": False, "name": "src",
            "url": "https://chromium.googlesource.com/chromium/src.git@" + version,
            "custom_vars": {"siso_version": MANIFEST["siso_version"],
                            "checkout_pgo_profiles": True, "source_tarball": False},
            "custom_deps": {}, "deps_file": "DEPS", "safesync_url": "",
        }]
        (chromium_dir / ".gclient").write_text("solutions = " + repr(solutions) + "\n")
        existing_checkout = chromium.exists()
        run([depot / "python-bin/python3", automate, f"--download-dir={download}",
             f"--depot-tools-dir={depot}", f"--branch={MANIFEST['chromium_branch']}",
             f"--checkout={MANIFEST['cef_revision']}", "--no-chromium-history",
             "--no-depot-tools-update", "--with-pgo-profiles", "--no-build", "--no-distrib",
             "--x64-build"], work, env)
        if existing_checkout:
            # Automation skips dependency sync when the shallow source revision
            # is unchanged. Explicitly resume any interrupted dependency fetch.
            run([depot / "gclient", "sync", "--nohooks", "--no-history"], chromium_dir, env)
            run([depot / "gclient", "runhooks"], chromium_dir, env)
        return

    verify_revision(depot, MANIFEST["depot_tools_revision"])
    if not (depot / "python3_bin_reldir.txt").exists():
        run([depot / "ensure_bootstrap"], work, env)
    verify_revision(chromium, MANIFEST["chromium_revision"])
    verify_revision(cef, MANIFEST["cef_revision"])
    if args.stage == "prepare":
        # Protect even a partially prepared tree from a destructive upstream resync.
        (work / "preparing.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
        for patch in MANIFEST["patches"]:
            if patch["repository"] == "cef":
                apply_once(cef, ROOT / patch["file"])
            else:
                # Register with CEF's own patch manager so project generation and
                # subsequent rebuilds retain the Chromium change.
                destination = cef / "patch/patches/miximus_native_handle.patch"
                shutil.copyfile(ROOT / patch["file"], destination)
                config = cef / "patch/patch.cfg"
                registration = "\npatches.append({'name': 'miximus_native_handle'})\n"
                text = config.read_text()
                if registration not in text:
                    config.write_text(text + registration)
        run([depot / "python-bin/python3", cef / "tools/gclient_hook.py"], cef, env)
        # Require our Chromium patch to have been applied, not skipped or rejected.
        patch = ROOT / MANIFEST["patches"][1]["file"]
        run(["git", "apply", "-p0", "--reverse", "--check", patch], chromium)
        (work / "prepared.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
        (work / "preparing.json").unlink()
        return

    if json.loads((work / "prepared.json").read_text()) != MANIFEST:
        raise RuntimeError("Build manifest changed; prepare this source tree again")
    for patch in MANIFEST["patches"]:
        is_cef = patch["repository"] == "cef"
        run(["git", "apply", "-p1" if is_cef else "-p0", "--reverse", "--check",
             ROOT / patch["file"]], cef if is_cef else chromium)
    if args.stage in ("build", "test"):
        # Ninja's job limit does not bound LLVM's internal ThinLTO pool.
        # LLVM counts Linux affinity CPUs when Chromium requests "all".
        cpus = sorted(os.sched_getaffinity(0))[:args.jobs]
        os.sched_setaffinity(0, cpus)
        print(f"Build CPU affinity: {cpus} (also bounds LLVM worker pools)", flush=True)
    if args.stage == "build":
        run([depot / "autoninja", "-C", "out/Release_GN_x64", f"-j{args.jobs}",
             "libcef", "cef_resources", "chrome_sandbox"], chromium, env)
        return

    if args.stage == "test":
        for patch in MANIFEST.get("test_patches", []):
            apply_once(chromium, ROOT / patch["file"])
        run([depot / "autoninja", "-C", "out/Release_GN_x64", f"-j{args.jobs}",
             "viz_unittests"], chromium, env)
        run([chromium / "out/Release_GN_x64/viz_unittests",
             "--gtest_filter=*FrameSinkVideoCapturerTest*"], chromium, env)
        return

    output = work / "distribution"
    name = "miximus_cef_linux64_native_handle_r1"
    if (output / name).exists():
        raise RuntimeError("Distribution already exists; use a fresh output directory")
    run([depot / "python-bin/python3", cef / "tools/make_distrib.py", f"--output-dir={output}",
         f"--distrib-subdir={name}", "--ninja-build", "--x64-build", "--allow-partial",
         "--no-symbols", "--no-docs", "--no-archive"], cef, env)
    sdk = output / name
    provenance = dict(MANIFEST)
    provenance["libcef_sha256"] = digest(sdk / "Release/libcef.so")
    (sdk / "miximus-source-build.json").write_text(json.dumps(provenance, indent=2) + "\n")
    archive = Path(shutil.make_archive(str(output / name), "bztar", output, name))
    archive_hash = digest(archive)
    (output / (name + ".sha256")).write_text(f"{archive_hash}  {archive.name}\n")
    artifact = json.loads((ROOT / "sdk.json").read_text())
    artifact.update(url="", sha256=archive_hash, archive_root=name,
                    patches=MANIFEST["patches"], source_build_revision=MANIFEST["revision"])
    (output / (name + ".json")).write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"Created {archive}; hardware qualification is still required.")


if __name__ == "__main__":
    main()
