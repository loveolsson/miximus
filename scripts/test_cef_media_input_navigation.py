#!/usr/bin/env python3
"""Exercise media-input retirement across a cross-origin renderer process swap."""

import argparse
import functools
import http.server
from pathlib import Path
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="miximus-navigation-") as profile:
        Path(profile, "blank.html").write_text("<!doctype html><title>Other origin</title>")
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=profile)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                subprocess.run(
                    [str(build / "src/nodes/cef/cef_media_input_session_probe"),
                     str(build / "cef"), profile, "--navigation",
                     f"http://127.0.0.1:{server.server_port}"],
                    check=True, timeout=90,
                )
            finally:
                server.shutdown()
                worker.join()


if __name__ == "__main__":
    main()
