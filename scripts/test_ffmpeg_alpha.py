#!/usr/bin/env python3
"""Qualify transparent video and linear premultiplied compositing in the real app."""
import base64
import json
import shutil
import struct
import subprocess
import threading
import time
import urllib.request
from http.server import ThreadingHTTPServer

from cef_test_process import PROCESS_CREATION_FLAGS, stop_process
from test_cef_inputs import API, ROOT, command, config
from test_ffmpeg_codecs import VideoPage, run

ALPHAS = [0, 1, 32, 64, 128, 192, 255]
RGB = [180, 90, 45]
WIDTH, HEIGHT = 280, 256


def generate(work):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg CLI required")
    row = b"".join(bytes(RGB + [alpha]) * (WIDTH // 7) for alpha in ALPHAS)
    raw = work / "alpha.rgba"
    raw.write_bytes(row * HEIGHT * 25)
    raw16 = work / "alpha.gbrap16le"
    alpha_row = b"".join(struct.pack("<H", alpha * 257) * (WIDTH // 7) for alpha in ALPHAS)
    planes = b"".join(struct.pack("<H", RGB[c] * 257) * WIDTH * HEIGHT for c in [1, 2, 0])
    raw16.write_bytes((planes + alpha_row * HEIGHT) * 25)
    videos = []
    for name, extension, codec, pixel_format, extra in [
        ("qtrle", "mov", "qtrle", "argb", []),
        ("prores4444", "mov", "prores_ks", "yuva444p10le", ["-profile:v", "4", "-alpha_bits", "16"]),
        ("vp9-alpha", "webm", "libvpx-vp9", "yuva420p", ["-lossless", "1", "-auto-alt-ref", "0"]),
        ("vp8-alpha", "webm", "libvpx", "yuva420p", ["-auto-alt-ref", "0", "-b:v", "1M"]),
        ("ffv1-alpha16", "mkv", "ffv1", "gbrap16le", ["-level", "3"]),
    ]:
        path = work / f"{name}.{extension}"
        source_format = "gbrap16le" if codec == "ffv1" else "rgba"
        filters = f"format={pixel_format}" if codec in ["ffv1", "qtrle"] else f"scale=in_range=full:out_color_matrix=bt709,format={pixel_format}"
        run(ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pixel_format", source_format, "-video_size",
            f"{WIDTH}x{HEIGHT}", "-framerate", "25", "-i", str(raw16 if codec == "ffv1" else raw), "-vf",
            filters, "-c:v", codec,
            "-threads", "2", "-color_trc", "iec61966-2-1", "-color_primaries", "bt709",
            "-colorspace", "bt709", *extra, str(path))
        # Qualify the encoder fixtures independently before testing the app.
        forced_decoder = ["-c:v", codec] if codec in ["libvpx", "libvpx-vp9"] else []
        reference_filter = ["-vf", "scale=in_color_matrix=bt709"] if pixel_format.startswith("yuva") else []
        decoded = run(ffmpeg, "-v", "error", *forced_decoder, "-i", str(path), "-frames:v", "1",
                      *reference_filter, "-f", "rawvideo", "-pix_fmt", "rgba", "-").stdout
        for i, alpha in enumerate(ALPHAS):
            offset = ((HEIGHT // 4) * WIDTH + (i * 40 + 20)) * 4
            actual = decoded[offset:offset + 4]
            if max(abs(a - b) for a, b in zip(actual, RGB + [alpha])) > 2:
                raise RuntimeError(f"Invalid {name} encoder fixture: {list(actual)} vs {RGB + [alpha]}")
        videos.append((name, path))
    backgrounds = {}
    for name, value in [("black", 0), ("white", 255)]:
        raw = work / f"{name}.rgba"
        raw.write_bytes(bytes([value, value, value, 255]) * WIDTH * HEIGHT * 25)
        path = work / f"{name}.mov"
        run(ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pixel_format", "rgba", "-video_size",
            f"{WIDTH}x{HEIGHT}", "-framerate", "25", "-i", str(raw), "-c:v", "qtrle", "-pix_fmt", "argb",
            "-color_trc", "iec61966-2-1", "-color_primaries", "bt709", str(path))
        backgrounds[name] = path
    return videos, backgrounds


def expected_pixel(index, background):
    alpha = ALPHAS[index] / 255
    if background == "transparent":
        return (RGB if alpha else [0, 0, 0]) + [ALPHAS[index]]
    result = []
    for channel in RGB:
        encoded = channel / 255
        linear = encoded / 12.92 if encoded <= .04045 else ((encoded + .055) / 1.055) ** 2.4
        linear = linear * alpha + (1 - alpha) * (background == "white")
        result.append(round(255 * (12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055)))
    return result + [255]


def main():
    try:
        config()
    except OSError:
        pass
    else:
        raise RuntimeError("An application already owns the API port")
    work = ROOT / "build/integration-tests" / time.strftime("ffmpeg-alpha-%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    print("Artifacts:", work, flush=True)
    videos, backgrounds = generate(work)
    server = ThreadingHTTPServer(("127.0.0.1", 0), VideoPage)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    def link(source, output, target, input_name):
        return dict(from_node=source, from_interface=output, to_node=target, to_interface=input_name)
    settings = work / "settings.json"
    settings.write_text(json.dumps(dict(schema_version=1, nodes=[
        dict(id="video", type="ffmpeg_playback_looping", options=dict(filename="")),
        dict(id="background", type="ffmpeg_playback_looping", options=dict(filename="")),
        dict(id="frame", type="framebuffer", options=dict(size=[WIDTH, HEIGHT])),
        dict(id="draw_background", type="draw_box", options={}),
        dict(id="draw_video", type="draw_box", options={}),
        dict(id="texture", type="framebuffer_to_texture", options={}),
        dict(id="browser", type="cef_browser", options=dict(size=[WIDTH, HEIGHT], url=f"http://127.0.0.1:{server.server_port}/")),
    ], connections=[link("frame", "fb", "draw_background", "fb_in"),
                    link("background", "texture", "draw_background", "tex"),
                    link("draw_background", "fb_out", "draw_video", "fb_in"),
                    link("video", "texture", "draw_video", "tex"),
                    link("draw_video", "fb_out", "texture", "fb"),
                    link("texture", "tex", "browser", "input_0")])), encoding="utf-8")
    records = []
    with (work / "app.log").open("w", encoding="utf-8") as log:
        app = subprocess.Popen([str(ROOT / "build/miximus"), "--settings", str(settings), "--stop-after", "180"],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, creationflags=PROCESS_CREATION_FLAGS)
        try:
            deadline = time.monotonic() + 40
            while True:
                if app.poll() is not None:
                    raise RuntimeError(f"App exited: {app.returncode}")
                try:
                    config()
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.1)
            for name, path in videos:
                for hardware in [False, True]:
                    command("update_node", id="video", options=dict(filename=str(path), attempt_hardware_decode=hardware))
                    for background in ["transparent", "black", "white"]:
                        command("update_node", id="background", options=dict(filename=str(backgrounds[background]) if background in backgrounds else ""))
                        deadline = time.monotonic() + 20
                        # Allow stale frames from the prior graph state to retire.
                        time.sleep(1)
                        while True:
                            if app.poll() is not None:
                                raise RuntimeError(f"App exited: {app.returncode}")
                            with urllib.request.urlopen(API + "/status", timeout=3) as response:
                                status = json.load(response).get("video", {})
                            with VideoPage.lock:
                                page = VideoPage.latest.copy()
                            if status.get("playback_error"):
                                raise RuntimeError(str(status))
                            error = 999
                            if page.get("pixels") and status.get("playback_loops", 0) >= 1:
                                errors = []
                                for i, actual in enumerate(page["pixels"]):
                                    expected = expected_pixel(i, background)
                                    for c in range(4):
                                        # Canvas unpremultiplication at near-zero alpha
                                        # amplifies 8-bit quantization. Opaque composites
                                        # below still check every RGB component there.
                                        if background == "transparent" and ALPHAS[i] in [1, 32] and c < 3:
                                            continue
                                        errors.append(abs(actual[c] - expected[c]))
                                error = max(errors)
                                if error <= 5:
                                    break
                            if time.monotonic() > deadline:
                                page.pop("png", None)
                                raise RuntimeError(f"{name}/{hardware}/{background}: error {error}; {status}; {page}")
                            time.sleep(.1)
                        label = f"{name}-{'hardware' if hardware else 'software'}-{background}"
                        (work / f"{label}.png").write_bytes(base64.b64decode(page.pop("png")))
                        records.append(dict(label=label, status=status, page=page, maximum_error=error))
                        (work / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
                        print(label, "pixel error", error, flush=True)
        finally:
            result = stop_process(app, timeout=20)
            server.shutdown()
            server.server_close()
        if result:
            raise RuntimeError(f"App exited: {result}")
    text = (work / "app.log").read_text(encoding="utf-8")
    if "VUID-" in text or "Validation Error" in text or "Application shutdown complete" not in text:
        raise RuntimeError("Inspect validation/shutdown log")
    print(f"Passed {len(records)} alpha playback/compositing cases: {work}", flush=True)


if __name__ == "__main__":
    main()
