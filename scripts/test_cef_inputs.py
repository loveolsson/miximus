#!/usr/bin/env python3
"""GPU-media input graph test against the regular CEF runtime."""

import argparse
import json
import pathlib
import signal
import subprocess
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = "http://127.0.0.1:7351/api/v1"


def config():
    with urllib.request.urlopen(API + "/config", timeout=2) as response:
        result = json.load(response)
    return result.get("config", result)


def command(topic, **values):
    data = json.dumps(
        dict(action="command", topic=topic, token=str(time.monotonic_ns()), **values)
    ).encode()
    request = urllib.request.Request(
        API + "/control", data=data, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(request, timeout=3) as response:
        body = response.read()
        result = json.loads(body) if body else {}
    if result.get("action") == "error":
        raise RuntimeError(str(result))
    return result


class Page(BaseHTTPRequestHandler):
    lock = threading.Lock()
    loads = 0
    inputs = 8
    latest = {}
    control = "run"

    def log_message(self, *_):
        pass

    def do_POST(self):
        report = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.lock:
            Page.latest = report
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path == "/control":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with self.lock:
                self.wfile.write(json.dumps(Page.control).encode())
            return
        if self.path != "/":
            self.send_error(404)
            return
        with self.lock:
            Page.loads += 1
            generation = Page.loads
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(
            (
                """<!doctype html>
<style>
  body {
    margin: 0;
    display: flex;
  }
  video {
    width: calc(100% / INPUT_COUNT);
    height: 100vh;
  }
</style>
<body>
  <script>
    const generation = GENERATION,
      samples = [],
      errors = [];
    for (let inputIndex = 0; inputIndex < INPUT_COUNT; inputIndex++) {
      const v = document.createElement("video");
      v.autoplay = true;
      v.muted = true;
      document.body.append(v);
      const sample = { width: 0, height: 0, presented: 0 };
      samples.push(sample);
      const observe = (now, m) => {
        sample.width = v.videoWidth;
        sample.height = v.videoHeight;
        sample.presented = m.presentedFrames;
        if (
          sample.resumeAt !== undefined &&
          sample.width === sample.resumeWidth &&
          sample.resumeMs === null
        )
          sample.resumeMs = now - sample.resumeAt;
        v.requestVideoFrameCallback(observe);
      };
      v.requestVideoFrameCallback(observe);
      miximus
        .getInputMediaStream({ inputIndex })
        .then((s) => {
          v.srcObject = s;
          return v.play();
        })
        .catch((e) => errors.push(String(e)));
    }
    let control = "run",
      applying = false;
    setInterval(async () => {
      if (applying) return;
      applying = true;
      try {
        const next = await (await fetch("/control")).json();
        if (next !== control) {
          for (const [inputIndex, v] of [
            ...document.querySelectorAll("video"),
          ].entries()) {
            if (next === "stop") {
              v.srcObject?.getTracks().forEach((t) => t.stop());
              v.srcObject = null;
            } else {
              Object.assign(samples[inputIndex], {
                resumeAt: performance.now(),
                resumeWidth: samples[inputIndex].width,
                resumeMs: null,
              });
              v.srcObject = await miximus.getInputMediaStream({ inputIndex });
              v.play().catch((e) => errors.push(String(e)));
            }
          }
          control = next;
        }
      } finally {
        applying = false;
      }
    }, 50);
    setInterval(
      () =>
        fetch("/report", {
          method: "POST",
          body: JSON.stringify({ generation, samples, errors }),
        }),
      100,
    );
  </script>
</body>
""".replace("GENERATION", str(generation)).replace("INPUT_COUNT", str(Page.inputs))
            ).encode()
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=int, choices=range(2, 9), default=8)
    parser.add_argument(
        "--connected-inputs",
        type=int,
        help="Initially connected sources; defaults to --inputs",
    )
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--browser-width", type=int, default=640)
    parser.add_argument("--browser-height", type=int, default=360)
    parser.add_argument("--warmup-seconds", type=float, default=3)
    parser.add_argument("--steady-seconds", type=float, default=0)
    args = parser.parse_args()
    if args.connected_inputs is None:
        args.connected_inputs = args.inputs
    if not 2 <= args.connected_inputs <= args.inputs:
        parser.error("Connected inputs must be 2..--inputs")
    if not (
        32 <= args.width <= 4096
        and 32 <= args.height <= 4096
        and 32 <= args.browser_width <= 4096
        and 32 <= args.browser_height <= 4096
        and 0 <= args.warmup_seconds <= 10
        and 0 <= args.steady_seconds <= 30
    ):
        parser.error(
            "Dimensions must be 32..4096, warmup 0..10 and steady interval 0..30 seconds"
        )
    try:
        config()
    except OSError:
        pass
    else:
        raise SystemExit("An app already serves the API; refusing to change its graph")
    work = ROOT / "build/integration-tests" / time.strftime("cef-inputs-%Y%m%d-%H%M%S")
    work.mkdir(parents=True, exist_ok=False)
    print("Artifacts:", work, flush=True)
    Page.inputs = args.inputs
    server = ThreadingHTTPServer(("127.0.0.1", 0), Page)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    connections = [
        dict(
            from_node=f"source-{i}",
            from_interface="texture",
            to_node="browser",
            to_interface=f"input_{i}",
        )
        for i in range(args.connected_inputs)
    ]
    nodes = [
        dict(
            id=f"source-{i}",
            type="test_pattern",
            options=dict(
                resolution=[args.width, args.height],
                pattern=["red_field", "green_field", "blue_field"][i % 3],
            ),
        )
        for i in range(args.connected_inputs)
    ]
    nodes.append(
        dict(
            id="browser",
            type="cef_browser",
            options=dict(
                size=[args.browser_width, args.browser_height],
                url=f"http://127.0.0.1:{server.server_port}/",
            ),
        )
    )
    settings = work / "settings.json"
    settings.write_text(
        json.dumps(dict(schema_version=1, nodes=nodes, connections=connections))
    )
    records = []
    with (work / "app.log").open("w") as log:
        app = subprocess.Popen(
            [
                str(ROOT / "build/miximus"),
                "--settings",
                str(settings),
                "--stop-after",
                "90",
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
        )

        def wait(label, predicate, allow_recovery=False, idle_graph=False):
            deadline = time.monotonic() + 20
            status, page = {}, {}
            while time.monotonic() < deadline:
                if app.poll() is not None:
                    raise RuntimeError(f"Application exited: {app.returncode}")
                with Page.lock:
                    page = Page.latest.copy()
                try:
                    statuses = config().get("status", {})
                    status = statuses.get("browser", {})
                    if page.get("errors"):
                        raise RuntimeError(str(page))
                    if status.get("cef_inputs_error") and not allow_recovery:
                        raise RuntimeError(str(status))
                    idle = all(
                        statuses.get("$app", {}).get(key) == 0
                        for key in (
                            "demanding_node_count",
                            "submitted_node_count",
                            "executed_node_count",
                        )
                    )
                    if predicate(status, page) and (not idle_graph or idle):
                        records.append(
                            dict(
                                label=label,
                                time=time.monotonic(),
                                status=status,
                                app=statuses.get("$app", {}),
                                page=page,
                            )
                        )
                        print(
                            label,
                            "delivered=",
                            status.get("cef_inputs_delivered"),
                            "drops=",
                            status.get("cef_inputs_drops"),
                            flush=True,
                        )
                        return
                except OSError:
                    pass
                time.sleep(0.05)
            raise RuntimeError(f"{label}: timeout: {status} {page}")

        try:
            wait(
                f"{args.inputs} inputs with no output consumer",
                lambda s, p: s.get("cef_inputs_active") == args.inputs
                and len(p.get("samples", [])) == args.inputs
                and all(
                    v["width"] == (args.width if i < args.connected_inputs else 16)
                    and v["presented"] >= (30 if i < args.connected_inputs else 1)
                    for i, v in enumerate(p["samples"])
                ),
            )
            if args.steady_seconds:
                if args.warmup_seconds:
                    time.sleep(args.warmup_seconds)
                    previous = records[-1]["page"]
                    wait(
                        "warmup complete",
                        lambda s, p: p.get("generation") == previous["generation"]
                        and all(
                            b["presented"] > a["presented"]
                            for a, b in zip(
                                previous["samples"][: args.connected_inputs],
                                p["samples"][: args.connected_inputs],
                            )
                        ),
                    )
                first = records[-1]
                time.sleep(args.steady_seconds)
                wait(
                    "steady interval",
                    lambda s, p: s.get("cef_inputs_delivered", 0)
                    > first["status"]["cef_inputs_delivered"],
                )
                last = records[-1]
                seconds = last["time"] - first["time"]
                rates = [
                    (b["presented"] - a["presented"]) / seconds
                    for a, b in zip(first["page"]["samples"], last["page"]["samples"])
                ]
                print("Presented frames/s per input:", rates, flush=True)
            with Page.lock:
                Page.control = "stop"
            wait(
                "all tracks stopped and exports freed",
                lambda s, p: s.get("cef_inputs_active") == 0
                and s.get("cef_inputs_export_bytes") == 0
                and s.get("cef_inputs_held") == 0
                and s.get("cef_inputs_reserved_bytes") == 0,
                idle_graph=True,
            )
            stopped = records[-1]["status"]["cef_inputs_submitted"]
            time.sleep(0.3)
            wait(
                "idle inputs submit no frames",
                lambda s, p: s.get("cef_inputs_submitted") == stopped,
            )
            with Page.lock:
                Page.control = "run"
            wait(
                "live tracks reacquired",
                lambda s, p: s.get("cef_inputs_active") == args.inputs
                and s.get("cef_inputs_submitted", 0)
                > stopped + args.connected_inputs * 10
                and all(v.get("resumeMs") is not None for v in p["samples"]),
            )
            print(
                "Reacquisition milliseconds per input:",
                [v["resumeMs"] for v in records[-1]["page"]["samples"]],
                flush=True,
            )
            command(
                "update_node",
                id=f"source-{args.connected_inputs - 1}",
                options={"resolution": [args.width // 2, args.height // 2]},
            )
            wait(
                "independent source resize",
                lambda s, p: p["samples"][args.connected_inputs - 1]["width"]
                == args.width // 2,
            )
            command("remove_connection", connection=connections[1])
            wait(
                "disconnected stream becomes transparent",
                lambda s, p: p["samples"][1]["width"] == 16,
            )
            command("add_connection", connection=connections[1])
            with Page.lock:
                loads = Page.loads
            for _ in range(6):
                command("node_action", id="browser", name="reload", payload={})
                time.sleep(0.15)
            with Page.lock:
                final_load = Page.loads
            wait(
                "rapid reload recovery",
                lambda s, p: s.get("cef_inputs_state") == "active"
                and not s.get("cef_inputs_error")
                and p.get("generation", 0) >= max(loads + 1, final_load)
                and len(p.get("samples", [])) == args.inputs
                and all(
                    v["presented"] >= (30 if i < args.connected_inputs else 1)
                    for i, v in enumerate(p["samples"])
                ),
                allow_recovery=True,
            )
            command("update_node", id="browser", options={"enabled": False})
            wait("disabled", lambda s, p: s.get("cef_state") == "stopped")
            with Page.lock:
                loads = Page.loads
            command("update_node", id="browser", options={"enabled": True})
            wait(
                "reenabled",
                lambda s, p: s.get("cef_inputs_state") == "active"
                and s.get("cef_inputs_delivered", 0) >= args.inputs * 30
                and p.get("generation", 0) > loads
                and len(p.get("samples", [])) == args.inputs
                and all(
                    v["presented"] >= (30 if i < args.connected_inputs else 1)
                    for i, v in enumerate(p["samples"])
                ),
            )
        finally:
            if app.poll() is None:
                app.send_signal(signal.SIGINT)
            try:
                result = app.wait(timeout=20)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
                raise
            finally:
                server.shutdown()
                server.server_close()
                (work / "samples.json").write_text(json.dumps(records, indent=2))
        if result:
            raise RuntimeError(f"Application exited: {result}")
    text = (work / "app.log").read_text()
    gpu_failures = (
        "Validation Error",
        "VUID-",
        "eglCreateImage failed",
        "Failed to create EGLImage",
        "ProduceSkiaGanesh failed",
        "Trying to produce a Skia representation from an incompatible backing",
    )
    if (
        any(error in text for error in gpu_failures)
        or "Application shutdown complete" not in text
    ):
        raise RuntimeError("Inspect application validation/shutdown log")
    print(
        "Graph demand, input streams, live edits, reload and shutdown passed",
        flush=True,
    )


if __name__ == "__main__":
    main()
