#!/usr/bin/env python3
"""Manual GPU-media input graph test. Select the experimental runtime via LD_LIBRARY_PATH."""
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
    data = json.dumps(dict(action="command", topic=topic, token=str(time.monotonic_ns()), **values)).encode()
    request = urllib.request.Request(API + "/control", data=data,
                                     headers={"Content-Type": "application/json"})
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

    def log_message(self, *_):
        pass

    def do_POST(self):
        report = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.lock:
            Page.latest = report
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
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
        self.wfile.write(('''<!doctype html><style>body{margin:0;display:flex}video{width:calc(100% / INPUT_COUNT);height:100vh}</style><body>
<script>
const generation = GENERATION, samples = [], errors = [];
for(let inputIndex=0;inputIndex<INPUT_COUNT;inputIndex++) {
    const v=document.createElement('video');v.autoplay=true;v.muted=true;document.body.append(v);
    const sample={width:0,height:0,presented:0};samples.push(sample);
    const observe=(now,m)=>{sample.width=v.videoWidth;sample.height=v.videoHeight;
        sample.presented=m.presentedFrames;v.requestVideoFrameCallback(observe)};
    v.requestVideoFrameCallback(observe);
    miximus.getInputMediaStream({inputIndex}).then(s=>{v.srcObject=s;return v.play()})
        .catch(e=>errors.push(String(e)));
}
setInterval(()=>fetch('/report',{method:'POST',body:JSON.stringify({generation,samples,errors})}),100);
</script>'''.replace("GENERATION", str(generation)).replace("INPUT_COUNT", str(Page.inputs))).encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=int, choices=range(2, 9), default=8)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--browser-width", type=int, default=640)
    parser.add_argument("--browser-height", type=int, default=360)
    parser.add_argument("--warmup-seconds", type=float, default=3)
    parser.add_argument("--steady-seconds", type=float, default=0)
    args = parser.parse_args()
    if not (32 <= args.width <= 4096 and 32 <= args.height <= 4096 and
            32 <= args.browser_width <= 4096 and 32 <= args.browser_height <= 4096 and
            0 <= args.warmup_seconds <= 10 and 0 <= args.steady_seconds <= 30):
        parser.error("Dimensions must be 32..4096, warmup 0..10 and steady interval 0..30 seconds")
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
    connections = [dict(from_node=f"source-{i}", from_interface="texture", to_node="browser",
                        to_interface=f"input_{i}") for i in range(args.inputs)]
    nodes = [dict(id=f"source-{i}", type="test_pattern", options=dict(
        resolution=[args.width, args.height], pattern=["red_field", "green_field", "blue_field"][i % 3])) for i in range(args.inputs)]
    nodes.append(dict(id="browser", type="cef_browser", options=dict(
        size=[args.browser_width, args.browser_height], url=f"http://127.0.0.1:{server.server_port}/")))
    settings = work / "settings.json"
    settings.write_text(json.dumps(dict(schema_version=1, nodes=nodes, connections=connections)))
    records = []
    with (work / "app.log").open("w") as log:
        app = subprocess.Popen([str(ROOT / "build/miximus"), "--settings", str(settings),
                                "--stop-after", "90"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        def wait(label, predicate, allow_recovery=False):
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
                    if predicate(status, page):
                        records.append(dict(label=label, time=time.monotonic(), status=status,
                                            app=statuses.get("$app", {}), page=page))
                        print(label, "delivered=", status.get("cef_inputs_delivered"),
                              "drops=", status.get("cef_inputs_drops"), flush=True)
                        return
                except OSError:
                    pass
                time.sleep(0.05)
            raise RuntimeError(f"{label}: timeout: {status} {page}")
        try:
            wait(f"{args.inputs} inputs with no output consumer", lambda s, p:
                 s.get("cef_inputs_active") == args.inputs and len(p.get("samples", [])) == args.inputs and
                 all(v["width"] == args.width and v["presented"] >= 30 for v in p["samples"]))
            if args.steady_seconds:
                if args.warmup_seconds:
                    time.sleep(args.warmup_seconds)
                    previous = records[-1]["page"]
                    wait("warmup complete", lambda s, p: p.get("generation") == previous["generation"] and
                         all(b["presented"] > a["presented"] for a, b in zip(previous["samples"], p["samples"])))
                first = records[-1]
                time.sleep(args.steady_seconds)
                wait("steady interval", lambda s, p: s.get("cef_inputs_delivered", 0) >
                     first["status"]["cef_inputs_delivered"])
                last = records[-1]
                seconds = last["time"] - first["time"]
                rates = [(b["presented"] - a["presented"]) / seconds
                         for a, b in zip(first["page"]["samples"], last["page"]["samples"])]
                print("Presented frames/s per input:", rates, flush=True)
            command("update_node", id=f"source-{args.inputs - 1}", options={"resolution": [args.width // 2, args.height // 2]})
            wait("independent source resize", lambda s, p: p["samples"][-1]["width"] == args.width // 2)
            command("remove_connection", connection=connections[1])
            delivered = records[-1]["status"]["cef_inputs_delivered"]
            wait("disconnected stream stays live", lambda s, p: s.get("cef_inputs_delivered", 0) > delivered + 100)
            command("add_connection", connection=connections[1])
            with Page.lock:
                loads = Page.loads
            for _ in range(6):
                command("node_action", id="browser", name="reload", payload={})
                time.sleep(0.15)
            with Page.lock:
                final_load = Page.loads
            wait("rapid reload recovery", lambda s, p: s.get("cef_inputs_state") == "active" and
                 not s.get("cef_inputs_error") and p.get("generation", 0) >= max(loads + 1, final_load) and
                 len(p.get("samples", [])) == args.inputs and all(v["presented"] >= 30 for v in p["samples"]),
                 allow_recovery=True)
            command("update_node", id="browser", options={"enabled": False})
            wait("disabled", lambda s, p: s.get("cef_state") == "stopped")
            with Page.lock:
                loads = Page.loads
            command("update_node", id="browser", options={"enabled": True})
            wait("reenabled", lambda s, p: s.get("cef_inputs_state") == "active" and
                 s.get("cef_inputs_delivered", 0) >= args.inputs * 30 and p.get("generation", 0) > loads and
                 len(p.get("samples", [])) == args.inputs and all(v["presented"] >= 30 for v in p["samples"]))
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
    if "Validation Error" in text or "VUID-" in text or "Application shutdown complete" not in text:
        raise RuntimeError("Inspect application validation/shutdown log")
    print("Graph demand, input streams, live edits, reload and shutdown passed", flush=True)


if __name__ == "__main__":
    main()
