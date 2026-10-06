"""Reproduce the Play environment failure and verify the local preset fixes it.

Uses a disposable CMake project; never changes the application build or presets.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    if os.name != "nt":
        raise RuntimeError("This regression requires Windows/MSVC")
    root = Path(__file__).resolve().parents[1]
    cache = (root / "build/CMakeCache.txt").read_text(encoding="utf-8")
    cmake = re.search(r"^CMAKE_COMMAND:INTERNAL=(.+)$", cache, re.MULTILINE).group(1)
    presets = json.loads((root / "CMakeUserPresets.json").read_text(encoding="utf-8"))
    configure = next(p for p in presets["configurePresets"] if p["name"] == "windows-cef-release")
    build = next(p for p in presets["buildPresets"] if p["name"] == "windows-cef-release")
    for preset in (configure, build):
        for key in ("INCLUDE", "LIB", "LIBPATH", "PATH"):
            if not preset.get("environment", {}).get(key):
                raise RuntimeError(f"{preset['name']} lacks explicit {key}; run scripts/windows_build.ps1 -SetupOnly")

    # Start without any inherited compiler setup, even when the caller is in a
    # Developer PowerShell. The preset must supply all build tool paths itself.
    environment = {k: v for k, v in os.environ.items()
                   if k.upper() not in ("INCLUDE", "LIB", "LIBPATH", "PATH", "CL", "_CL_", "LINK", "_LINK_")}
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    artifacts = root / "build/integration-tests/windows-build-environment"
    artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
    print(f"Logs: {work}", flush=True)
    (work / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.28)\nproject(play_environment LANGUAGES CXX)\n'
        'add_executable(probe probe.cpp)\ntarget_compile_features(probe PRIVATE cxx_std_20)\n'
        'target_include_directories(probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")\n',
        encoding="utf-8")
    (work / "probe.cpp").write_text(
        '#include <filesystem>\n#include <chrono>\n#include <type_traits>\n#include <windows.h>\n'
        'int main() { return std::filesystem::exists(std::filesystem::current_path()) '
        '&& GetCurrentProcessId() != 0 ? 0 : 1; }\n', encoding="utf-8")
    (work / "CMakePresets.json").write_text(json.dumps({
        "version": 6,
        "configurePresets": [{"name": "probe", "generator": "Ninja", "binaryDir": "${sourceDir}/out",
                              "cacheVariables": {"CMAKE_CXX_COMPILER": "cl", "CMAKE_BUILD_TYPE": "Release"},
                              "environment": configure["environment"]}],
        "buildPresets": [{"name": "probe", "configurePreset": "probe", "environment": build["environment"]}],
    }, indent=2), encoding="utf-8")

    def run(name, args):
        result = subprocess.run(args, cwd=work, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
        (work / f"{name}.log").write_text(result.stdout, encoding="utf-8")
        return result

    configured = run("configure", [cmake, "--preset", "probe"])
    if configured.returncode:
        raise RuntimeError(f"Configure failed: {configured.stdout}")
    # Same command shape as the failing CMake Tools Play build; the compiler is
    # already located by CMake, but INCLUDE/LIB are absent from this process.
    broken = run("negative-control", [cmake, "--build", str(work / "out"), "--target", "probe"])
    if broken.returncode == 0 or "C1083" not in broken.stdout:
        raise RuntimeError(f"Did not reproduce the missing-header failure: {broken.stdout}")
    fixed = run("preset-build", [cmake, "--build", "--preset", "probe"])
    if fixed.returncode or "Building CXX object" not in fixed.stdout or "Linking CXX executable" not in fixed.stdout:
        raise RuntimeError(f"Preset did not compile and link successfully: {fixed.stdout}")
    executed = run("execute", [str(work / "out/probe.exe")])
    if executed.returncode:
        raise RuntimeError(f"Compiled probe failed: {executed.stdout}")
    print("PASS: missing environment reproduces C1083; explicit preset compiles, links, and runs")


if __name__ == "__main__":
    main()
