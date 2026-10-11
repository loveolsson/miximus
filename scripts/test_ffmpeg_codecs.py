#!/usr/bin/env python3
"""Generate codec fixtures, then verify rendered pixels and looping in the real app.

Requires ffmpeg/ffprobe on PATH and the project's CEF-enabled application build.
Uses private settings, refuses an existing application, and writes JSON/PNG evidence.
--require-vulkan requires H.264 and HEVC to stay entirely on the GPU.
--require-hardware requires H.264 and HEVC hardware decoding with GPU-resident ingestion.
"""
import argparse
import base64
import json
import pathlib
import shutil
import subprocess
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cef_test_process import PROCESS_CREATION_FLAGS, stop_process
from test_cef_inputs import API, ROOT, command, config


class VideoPage(BaseHTTPRequestHandler):
    latest = {}
    lock = threading.Lock()

    def log_message(self, *_):
        pass

    def do_POST(self):
        sample = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.lock:
            self.__class__.latest = sample
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(b'''<!doctype html><body style="margin:0;background:black">
<video autoplay muted style="width:100vw;height:100vh"></video><script>
const video = document.querySelector('video'), canvas = document.createElement('canvas');
let presented = 0, error = '';
const observe = () => { ++presented; video.requestVideoFrameCallback(observe); };
video.requestVideoFrameCallback(observe);
miximus.getInputMediaStream({inputIndex:0}).then(s => {video.srcObject=s; return video.play();})
 .catch(e=>error=String(e));
setInterval(async()=>{
 try {
  const width=video.videoWidth, height=video.videoHeight;
  if(!width || !height) return;
  canvas.width=width; canvas.height=height;
  const ctx=canvas.getContext('2d',{willReadFrequently:true});
  ctx.drawImage(video,0,0);
  const pixels=Array.from({length:7},(_,i)=>Array.from(ctx.getImageData(
      Math.floor(width*(i+.5)/7),Math.floor(height/4),1,1).data));
  await fetch('/',{method:'POST',body:JSON.stringify({width,height,presented,pixels,error,
      motion:Array.from(ctx.getImageData(Math.floor(width/2),height-3,1,1).data),
      png:canvas.toDataURL('image/png').split(',')[1]})});
 } catch(e) {error=String(e);}
},200);
</script>''')


def run(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def display_sample(value):
    """The fixture is Rec.709 encoded; the browser canvas readback is sRGB."""
    encoded = value / 255
    linear = encoded / 4.5 if encoded < .081 else ((encoded + .099) / 1.099) ** (1 / .45)
    return round(255 * (12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055))


def fixtures(work):
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise RuntimeError("ffmpeg and ffprobe are required to generate reproducible fixtures")
    specs = [
        ("h264", "mp4", "libx264", "yuv420p", 384, 216, []),
        ("hevc10", "mp4", "libx265", "yuv420p10le", 320, 180, ["-x265-params", "log-level=error:pools=2"]),
        ("vp9", "webm", "libvpx-vp9", "yuv420p", 288, 162, ["-deadline", "realtime", "-cpu-used", "6"]),
        ("av1", "mkv", "libaom-av1", "yuv420p", 280, 160, ["-cpu-used", "8"]),
        ("prores", "mov", "prores_ks", "yuv422p10le", 256, 144, ["-profile:v", "3"]),
        ("ffv1-rgb16", "mkv", "ffv1", "gbrp16le", 321, 181, ["-level", "3"]),
    ]
    result = []
    for name, extension, codec, pixel_format, width, height, options in specs:
        path = work / f"{name}.{extension}"
        # Keep the upper color bars static for reference comparisons, while a
        # changing lower band proves decoded pixels advance through the graph.
        filters = ("geq=lum='if(gte(Y,H-24),16+mod(N*4,220),p(X,Y))':"
                   "cb='if(gte(Y,H-12),128,p(X,Y))':cr='if(gte(Y,H-12),128,p(X,Y))',"
                   f"scale={width}:{height}:in_color_matrix=bt601:out_color_matrix=bt709,format={pixel_format}")
        args = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                "smptebars=size=384x216:rate=25:duration=2", "-f", "lavfi", "-i", "sine=duration=2",
                "-map", "0:v", "-map", "1:a", "-vf", filters, "-c:v", codec, "-threads", "2",
                "-g", "25", "-pix_fmt", pixel_format, "-color_primaries", "bt709", "-color_trc", "bt709",
                "-colorspace", "bt709", "-c:a", "libopus" if extension == "webm" else "aac"]
        run(*(args + options + [str(path)]))
        probe = json.loads(run(ffprobe, "-v", "error", "-show_streams", "-of", "json", str(path)).stdout)
        video = next(stream for stream in probe["streams"] if stream["codec_type"] == "video")
        raw = run(ffmpeg, "-v", "error", "-i", str(path), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-").stdout
        pixels = []
        for i in range(7):
            offset = ((height // 4) * width + int(width * (i + .5) / 7)) * 3
            pixels.append([display_sample(value) for value in raw[offset:offset + 3]])
        result.append(dict(name=name, path=str(path), width=width, height=height, reference=pixels, stream=video))
    (work / "fixtures.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-vulkan", action="store_true")
    parser.add_argument("--require-hardware", action="store_true")
    parser.add_argument("--use-cuda", action="store_true")
    args = parser.parse_args()
    try:
        config()
    except OSError:
        pass
    else:
        raise RuntimeError("A Miximus instance already owns the API port")
    work = ROOT / "build/integration-tests" / time.strftime("ffmpeg-codecs-%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    print("Artifacts:", work, flush=True)
    videos = fixtures(work)
    server = ThreadingHTTPServer(("127.0.0.1", 0), VideoPage)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    settings = work / "settings.json"
    settings.write_text(json.dumps(dict(schema_version=1, nodes=[
        dict(id="video", type="ffmpeg_playback_looping", options=dict(filename="")),
        dict(id="browser", type="cef_browser", options=dict(size=[384,216], url=f"http://127.0.0.1:{server.server_port}/")),
    ], connections=[dict(from_node="video", from_interface="texture", to_node="browser", to_interface="input_0")])), encoding="utf-8")
    records = []
    with (work / "app.log").open("w", encoding="utf-8") as log:
        app = subprocess.Popen([str(ROOT / "build/miximus"), "--settings", str(settings), "--stop-after", "240"]
                               + (["--use-cuda"] if args.use_cuda else []), cwd=ROOT, stdout=log,
                               stderr=subprocess.STDOUT, creationflags=PROCESS_CREATION_FLAGS)
        try:
            deadline = time.monotonic() + 40
            while True:
                if app.poll() is not None: raise RuntimeError(f"App exited: {app.returncode}")
                try:
                    config()
                    break
                except OSError:
                    if time.monotonic() > deadline: raise
                    time.sleep(.1)
            for video in videos:
                software_pixels = None
                for hardware in [False, True]:
                    command("update_node", id="video", options=dict(filename=""))
                    time.sleep(.15)
                    with VideoPage.lock:
                        initial_presented = VideoPage.latest.get("presented", 0)
                    command("update_node", id="video", options=dict(filename=video["path"], attempt_hardware_decode=hardware))
                    deadline = time.monotonic() + 35
                    motion_samples = set()
                    while True:
                        if app.poll() is not None: raise RuntimeError(f"App exited: {app.returncode}")
                        with urllib.request.urlopen(API + "/status", timeout=3) as response:
                            status = json.load(response).get("video", {})
                        with VideoPage.lock:
                            page = VideoPage.latest.copy()
                        if (status.get("playback_position_seconds", 0) > .5 and page.get("width") == video["width"]
                                and page.get("height") == video["height"] and page.get("motion")):
                            motion_samples.add(tuple(page["motion"]))
                        if status.get("playback_error") or page.get("error"):
                            raise RuntimeError(str(dict(status=status, page=page)))
                        if (status.get("connected") and status.get("playback_loops", 0) >= 2
                                and status.get("playback_position_seconds", 0) > 3.9
                                and page.get("width") == video["width"] and page.get("height") == video["height"]
                                and len(motion_samples) >= 4
                                and page.get("presented", 0) > initial_presented + 10):
                            break
                        if time.monotonic() > deadline:
                            page.pop("png", None)
                            raise RuntimeError(f"Playback timed out: {video['name']}, {status}, {page}")
                        time.sleep(.1)
                    label = video["name"] + ("-hardware" if hardware else "-software")
                    (work / f"{label}.png").write_bytes(base64.b64decode(page.pop("png")))
                    error = max(abs(actual - expected) for a,b in zip(page["pixels"], video["reference"]) for actual,expected in zip(a[:3],b))
                    if error > 5: raise RuntimeError(f"{label}: rendered/reference pixel error {error}; {page['pixels']} vs {video['reference']}")
                    if hardware:
                        if status.get("playback_backend") != "software" and status.get("playback_transfer_path") not in ["Vulkan device-local components", "D3D11 shared GPU texture to Vulkan", "VAAPI shared GPU surface to Vulkan"]:
                            raise RuntimeError(f"{label}: hardware decode must stay GPU resident: {status}")
                        delta = max(abs(a-b) for x,y in zip(page["pixels"],software_pixels) for a,b in zip(x,y))
                        if delta > 3: raise RuntimeError(f"{label}: hardware/software pixel mismatch {delta}")
                        if args.require_hardware and video["name"] in ["h264", "hevc10"] and status.get("playback_backend") == "software":
                            raise RuntimeError(f"{label}: hardware decoding required: {status}")
                        if args.require_vulkan and video["name"] in ["h264", "hevc10"]:
                            if status.get("playback_backend") != "vulkan" or status.get("playback_transfer_path") != "Vulkan device-local components":
                                raise RuntimeError(f"{label}: GPU-resident decoding required: {status}")
                    else:
                        software_pixels = page["pixels"]
                        if status.get("playback_backend") != "software": raise RuntimeError("Software option was ignored")
                    record = dict(label=label, status=status, page=page, maximum_reference_error=error,
                                  distinct_motion_samples=len(motion_samples))
                    records.append(record)
                    print(label, status["playback_backend"], status["playback_transfer_path"], "pixel error", error, flush=True)
                    (work / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        finally:
            result = stop_process(app, timeout=20)
            server.shutdown()
            server.server_close()
        if result: raise RuntimeError(f"Application exited with {result}")
    log_text = (work / "app.log").read_text(encoding="utf-8")
    if "Unable to find device with index" in log_text:
        raise RuntimeError("FFmpeg Vulkan device selection failed")
    if "VUID-" in log_text or "Validation Error" in log_text or "Application shutdown complete" not in log_text:
        raise RuntimeError("Inspect GPU validation/shutdown log")
    print(f"Passed {len(records)} playback cases; rendered PNGs and JSON evidence: {work}", flush=True)


if __name__ == "__main__":
    main()
