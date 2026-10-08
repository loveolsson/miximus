#!/usr/bin/env python3
"""Exercise asynchronous image loading through two real CEF texture inputs."""

import argparse
import json
import struct
import subprocess
import threading
import time
import zlib
from http.server import ThreadingHTTPServer

from cef_test_process import PROCESS_CREATION_FLAGS, stop_process
from test_cef_inputs import Page, ROOT, command, config


def write_png(path, width, height):
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    pixels = (b"\0" + bytes([255, 64, 0, 128]) * width) * height
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--use-cuda", action="store_true")
    args = parser.parse_args()
    try:
        config()
    except OSError:
        pass
    else:
        raise SystemExit("An app already serves the API; refusing to change its graph")
    work = ROOT / "build/integration-tests" / time.strftime("image-node-%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    print("Artifacts:", work, flush=True)
    first = work / "image-å.png"
    second = work / "replacement.png"
    write_png(first, 96, 64)
    write_png(second, 128, 80)
    invalid = work / "invalid.png"
    invalid.write_text("not an image")
    Page.inputs = 2
    server = ThreadingHTTPServer(("127.0.0.1", 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    settings = work / "settings.json"
    settings.write_text(
        json.dumps(
            dict(
                schema_version=1,
                nodes=[
                    dict(id=f"image-{i}", type="image", options=dict(file_path=str(first)))
                    for i in range(2)
                ]
                + [
                    dict(
                        id="browser",
                        type="cef_browser",
                        options=dict(
                            url=f"http://127.0.0.1:{server.server_port}/", size=[640, 360]
                        ),
                    )
                ],
                connections=[
                    dict(
                        from_node=f"image-{i}", from_interface="texture",
                        to_node="browser", to_interface=f"input_{i}",
                    )
                    for i in range(2)
                ],
            )
        )
    )
    records = []
    with (work / "app.log").open("w") as log:
        app = subprocess.Popen(
            [str(ROOT / "build/miximus"), "--settings", str(settings), "--stop-after", "90"]
            + (["--use-cuda"] if args.use_cuda else []),
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            creationflags=PROCESS_CREATION_FLAGS,
        )

        def wait(label, widths):
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if app.poll() is not None:
                    raise RuntimeError(f"Application exited: {app.returncode}")
                with Page.lock:
                    page = Page.latest.copy()
                if page.get("errors"):
                    raise RuntimeError(str(page))
                samples = page.get("samples", [])
                if len(samples) == 2 and [s["width"] for s in samples] == widths:
                    records.append(dict(label=label, page=page, config=config()))
                    print(label, widths, flush=True)
                    return
                time.sleep(.05)
            raise RuntimeError(f"{label}: timeout: {page}")

        def select(path):
            command("update_node", id="image-0", options=dict(file_path=str(path)))

        try:
            wait("PNG and Unicode path loaded", [96, 96])
            select(second)
            wait("replacement published", [128, 96])
            select(invalid)
            time.sleep(.5)
            wait("decode failure retains previous image", [128, 96])
            select("")
            wait("clearing path releases output", [16, 96])
            for _ in range(12):
                select(second)
                select(first)
            wait("rapid replacement publishes final request", [96, 96])
            large = work / "large.png"
            write_png(large, 2048, 2048)
            select(large)
            command("remove_node", id="image-0")
            wait("removal while loading retires safely", [16, 96])
        finally:
            result = stop_process(app, timeout=20)
            server.shutdown()
            server.server_close()
            (work / "samples.json").write_text(json.dumps(records, indent=2))
        if result:
            raise RuntimeError(f"Application exited: {result}")
    text = (work / "app.log").read_text()
    if "Application shutdown complete" not in text or "VUID-" in text or "Validation Error" in text:
        raise RuntimeError("Inspect application validation/shutdown log")
    if text.count(f"Image '{invalid}' failed:") != 1:
        raise RuntimeError("Decode failure should be logged once per failed selection")
    print("Image loading, replacement, failure, cancellation and shutdown passed", flush=True)


if __name__ == "__main__":
    main()
