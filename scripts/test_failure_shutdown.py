#!/usr/bin/env python3
"""Check shutdown/recovery with private settings and a BUILD_TESTING executable."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/miximus").resolve()
    with socket.socket() as connection:
        if connection.connect_ex(("127.0.0.1", 7351)) == 0:
            raise RuntimeError("Another application owns port 7351; refusing to interfere")
    root = Path(tempfile.mkdtemp(prefix="miximus-failure-shutdown-"))
    print(f"Logs and private settings: {root}", flush=True)
    for mode in ("normal", "malformed", "render", "configuration", "shutdown", "park"):
        settings = root / f"{mode}.json"
        contents = "{ invalid settings" if mode == "malformed" else json.dumps(
            {"schema_version": 1, "nodes": [], "connections": []}
        )
        settings.write_text(contents)
        environment = os.environ.copy()
        environment.pop("MIXIMUS_TEST_SHUTDOWN_FAILURE", None)
        if mode in ("render", "configuration", "shutdown", "park"):
            environment["MIXIMUS_TEST_SHUTDOWN_FAILURE"] = mode
        with (root / f"{mode}.log").open("w") as log:
            result = subprocess.run(
                [str(binary), "--settings", str(settings), "--stop-after", "0.2"],
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=85,
                check=False,
            )
        expected = 0 if mode == "normal" else 1
        if result.returncode != expected:
            raise AssertionError(f"{mode}: exit {result.returncode}, expected {expected}; see {root}")
        recovered = list(root.glob(f"{mode}.json.recovery-*.json"))
        if mode == "normal":
            assert len(json.loads(settings.read_text())["nodes"]) == 1
            assert not recovered
        else:
            assert settings.read_text() == contents, f"{mode}: original settings changed"
            if mode == "malformed":
                assert not recovered, "Partially loaded settings must not be checkpointed"
            else:
                assert len(recovered) == 1, f"{mode}: expected one unique recovery file"
                assert len(json.loads(recovered[0].read_text())["nodes"]) == 1
                assert f"Recovery settings saved to {recovered[0]}" in (root / f"{mode}.log").read_text()
        if mode in ("render", "configuration", "shutdown"):
            assert "Application shutdown complete" in (root / f"{mode}.log").read_text()
        print(f"PASS: {mode}", flush=True)


if __name__ == "__main__":
    main()
