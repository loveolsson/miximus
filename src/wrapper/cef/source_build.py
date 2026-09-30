#!/usr/bin/env python3
"""Explicit, resumable Linux/Windows CEF SDK build. Never called by application CMake."""

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
PLATFORM = "windows64" if sys.platform == "win32" else "linux64"
MANIFEST.update(MANIFEST.pop("platforms", {}).get(PLATFORM, {}))


def run(args, cwd, env=None):
    print("Running:", " ".join(map(str, args)), flush=True)
    command = list(map(str, args))
    if sys.platform == "win32" and Path(command[0]).suffix.lower() == ".bat":
        command = [os.environ["COMSPEC"], "/d", "/c", *command]
    subprocess.run(command, cwd=cwd, env=env, check=True)


def bootstrap(depot, work, env):
    script = "bootstrap/win_tools.bat" if sys.platform == "win32" else "ensure_bootstrap"
    run([depot / script], work, env)


def depot_python(depot):
    if sys.platform == "win32":
        relative = (depot / "python3_bin_reldir.txt").read_text().strip()
        return depot / relative / "python3.exe"
    return depot / "python-bin/python3"


def depot_command(depot, name):
    return depot / (name + ".bat" if sys.platform == "win32" else name)


def limit_build_affinity(jobs):
    # Preserve the Linux ThinLTO memory bound. On Windows, let the scheduler
    # spread compiler jobs across physical cores rather than pinning SMT siblings.
    if sys.platform == "linux":
        cpus = sorted(os.sched_getaffinity(0))[:jobs]
        os.sched_setaffinity(0, cpus)
        print(f"Build CPU affinity: {cpus} (also bounds LLVM worker pools)", flush=True)


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
    concurrency = parser.add_mutually_exclusive_group()
    concurrency.add_argument("--jobs", type=int, default=6)
    concurrency.add_argument("--auto-jobs", action="store_true",
                             help="Let autoninja choose concurrency without restricting CPU affinity")
    parser.add_argument("--no-archive", action="store_true", help="Package a local SDK without compressing an archive")
    args = parser.parse_args()
    if args.no_archive and args.stage != "package":
        parser.error("--no-archive is only valid for package")
    if sys.platform not in ("linux", "win32") or args.jobs < 1:
        parser.error("Requires Linux or Windows and a positive job count")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    depot = work / "depot_tools"
    download = work / "work"
    chromium = download / "chromium/src"
    cef = chromium / "cef"
    env = dict(os.environ)
    env.update(DEPOT_TOOLS_UPDATE="0", GN_DEFINES=MANIFEST["gn_defines"])
    if sys.platform == "win32":
        if len(str(work)) >= 35 or not str(work).isascii() or any(c in str(work) for c in ' &|<>^%!'):
            parser.error("Windows work directory must be a short ASCII path without spaces or shell metacharacters")
        env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
        env["GN_OUT_CONFIGS"] = "Release_GN_x64"
        # Keep source/patch bytes intact without changing the user's global Git settings.
        count = int(env.get("GIT_CONFIG_COUNT", "0"))
        for key, value in (("core.autocrlf", "false"), ("core.longpaths", "true")):
            env[f"GIT_CONFIG_KEY_{count}"] = key
            env[f"GIT_CONFIG_VALUE_{count}"] = value
            count += 1
        env["GIT_CONFIG_COUNT"] = str(count)
    env["PATH"] = str(depot) + os.pathsep + env["PATH"]
    for patch in MANIFEST["patches"] + MANIFEST.get("test_patches", []) + MANIFEST.get("sync_patches", []):
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
        bootstrap(depot, work, env)
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
            "custom_deps": {},
            "deps_file": "DEPS", "safesync_url": "",
        }]
        (chromium_dir / ".gclient").write_text("solutions = " + repr(solutions) + "\n")
        if MANIFEST.get("sync_patches"):
            # Fetch the pinned primary tree before automation can fetch its DEPS.
            # This omits unused binary test fixtures without downloading them first.
            if not chromium.exists():
                run(["git", "clone", "--depth=1", "--branch", version,
                     "https://chromium.googlesource.com/chromium/src.git", chromium], chromium_dir, env)
            verify_revision(chromium, MANIFEST["chromium_revision"])
            for patch in MANIFEST["sync_patches"]:
                apply_once(chromium, ROOT / patch["file"])
        existing_checkout = chromium.exists()
        run([depot_python(depot), automate, f"--download-dir={download}",
             f"--depot-tools-dir={depot}", f"--branch={MANIFEST['chromium_branch']}",
             f"--checkout={MANIFEST['cef_revision']}", "--no-chromium-history",
             "--no-depot-tools-update", "--with-pgo-profiles", "--no-build", "--no-distrib",
             "--x64-build"], work, env)
        if existing_checkout:
            # Automation skips dependency sync when the shallow source revision
            # is unchanged. Explicitly resume any interrupted dependency fetch.
            gclient = depot_command(depot, "gclient")
            run([gclient, "sync", "--nohooks", "--no-history"], chromium_dir, env)
            run([gclient, "runhooks"], chromium_dir, env)
        return

    verify_revision(depot, MANIFEST["depot_tools_revision"])
    if not (depot / "python3_bin_reldir.txt").exists():
        bootstrap(depot, work, env)
    verify_revision(chromium, MANIFEST["chromium_revision"])
    verify_revision(cef, MANIFEST["cef_revision"])
    for patch in MANIFEST.get("sync_patches", []):
        run(["git", "apply", "-p1", "--reverse", "--check", ROOT / patch["file"]], chromium)
    # Test overlays may adapt tests shipped inside a production patch. Remove
    # exactly matching overlays before verifying/preparing production sources;
    # the test stage reapplies them after verification.
    test_source_times = {}
    for patch in MANIFEST.get("test_patches", []):
        for line in (ROOT / patch["file"]).read_text().splitlines():
            if line.startswith("+++ b/"):
                source = chromium / line.removeprefix("+++ b/")
                if source.is_file():
                    test_source_times[source] = (digest(source), source.stat())
    for patch in reversed(MANIFEST.get("test_patches", [])):
        patch_path = ROOT / patch["file"]
        applied = subprocess.run(
            ["git", "apply", "-p1", "--reverse", "--check", str(patch_path)],
            cwd=chromium, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
        if applied:
            run(["git", "apply", "-p1", "--reverse", patch_path], chromium)
    if args.stage == "prepare":
        # Protect even a partially prepared tree from a destructive upstream resync.
        (work / "preparing.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
        for patch in MANIFEST["patches"]:
            if patch["repository"] == "cef":
                apply_once(cef, ROOT / patch["file"])
            else:
                # Register with CEF's own patch manager so project generation and
                # subsequent rebuilds retain the Chromium change.
                name = patch.get("registration_name", "miximus_native_handle")
                destination = cef / f"patch/patches/{name}.patch"
                shutil.copyfile(ROOT / patch["file"], destination)
                config = cef / "patch/patch.cfg"
                registration = "\npatches.append({'name': '" + name + "'})\n"
                text = config.read_text()
                if registration not in text:
                    config.write_text(text + registration)
        run([depot_python(depot), cef / "tools/gclient_hook.py"], cef, env)
        # Require our Chromium patch to have been applied, not skipped or rejected.
        for patch in MANIFEST["patches"]:
            if patch["repository"] == "chromium":
                run(["git", "apply", "-p0", "--reverse", "--check", ROOT / patch["file"]], chromium)
        (work / "prepared.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
        (work / "preparing.json").unlink()
        return

    # Test patches are applied by the test stage, not project preparation.
    # Updating a test mock does not invalidate the prepared production build.
    prepared = json.loads((work / "prepared.json").read_text())
    production_manifest = {key: value for key, value in MANIFEST.items() if key != "test_patches"}
    prepared_production = {key: value for key, value in prepared.items() if key != "test_patches"}
    if prepared_production != production_manifest:
        raise RuntimeError("Build manifest changed; prepare this source tree again")
    for patch in MANIFEST["patches"]:
        is_cef = patch["repository"] == "cef"
        run(["git", "apply", "-p1" if is_cef else "-p0", "--reverse", "--check",
             ROOT / patch["file"]], cef if is_cef else chromium)
    job_args = [] if args.auto_jobs else [f"-j{args.jobs}"]
    if args.stage in ("build", "test") and not args.auto_jobs:
        limit_build_affinity(args.jobs)
    if args.stage == "build":
        # The pinned Windows distribution packages the sandbox bootstrap binaries.
        sandbox_targets = ["bootstrap", "bootstrapc"] if sys.platform == "win32" else ["chrome_sandbox"]
        run([depot_command(depot, "autoninja"), "-C", "out/Release_GN_x64", *job_args,
             "libcef", "cef_resources", *sandbox_targets], chromium, env)
        return

    if args.stage == "test":
        for patch in MANIFEST.get("test_patches", []):
            apply_once(chromium, ROOT / patch["file"])
        # Restoring identical test overlays must not force GN regeneration or
        # recompile unrelated tests. Changed files retain their new timestamps.
        for source, (previous_hash, previous_stat) in test_source_times.items():
            if source.is_file() and digest(source) == previous_hash:
                os.utime(source, ns=(previous_stat.st_atime_ns, previous_stat.st_mtime_ns))
        run([depot_command(depot, "autoninja"), "-C", "out/Release_GN_x64", *job_args,
             "viz_unittests", "cef_external_video_source_unittests"], chromium, env)
        suffix = ".exe" if sys.platform == "win32" else ""
        run([chromium / f"out/Release_GN_x64/viz_unittests{suffix}",
             "--gtest_filter=*FrameSinkVideoCapturerTest*"], chromium, env)
        run([chromium / f"out/Release_GN_x64/cef_external_video_source_unittests{suffix}",
             "--gtest_filter=ExternalVideoSourceTest.*", "--test-launcher-jobs=1"], chromium, env)
        return

    output = work / "distribution"
    platform = "windows64" if sys.platform == "win32" else "linux64"
    name = f"miximus_cef_{platform}_native_handle_r{MANIFEST['revision']}"
    if (output / name).exists():
        raise RuntimeError("Distribution already exists; use a fresh output directory")
    run([depot_python(depot), cef / "tools/make_distrib.py", f"--output-dir={output}",
         f"--distrib-subdir={name}", "--ninja-build", "--x64-build", "--allow-partial",
         "--no-symbols", "--no-docs", "--no-archive"], cef, env)
    sdk = output / name
    # Our private ABI is deliberately outside CEF's generated public API list.
    # Package its authoritative header so consumers never duplicate its layout.
    shutil.copy2(cef / "include/internal/cef_miximus_media_input.h",
                 sdk / "include/internal/cef_miximus_media_input.h")
    provenance = dict(MANIFEST)
    library = "libcef.dll" if sys.platform == "win32" else "libcef.so"
    provenance["libcef_sha256"] = digest(sdk / "Release" / library)
    (sdk / "miximus-source-build.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if args.no_archive:
        print(f"Created local SDK {sdk}; configure MIXIMUS_CEF_ROOT to this directory.")
        return
    archive_format = "zip" if sys.platform == "win32" else "bztar"
    archive = Path(shutil.make_archive(str(output / name), archive_format, output, name))
    archive_hash = digest(archive)
    (output / (name + ".sha256")).write_text(f"{archive_hash}  {archive.name}\n")
    artifact = json.loads((ROOT / "sdk.json").read_text())
    artifact.update(url="", sha256=archive_hash, archive_root=name,
                    patches=MANIFEST["patches"], source_build_revision=MANIFEST["revision"])
    if sys.platform == "win32":
        artifact.update(platform=platform)
        artifact.pop("minimum_os", None) # Stock metadata describes Linux only.
    (output / (name + ".json")).write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"Created {archive}; hardware qualification is still required.")


if __name__ == "__main__":
    main()
