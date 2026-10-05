#!/usr/bin/env python3
"""Exercise screen failure status and explicit reconfiguration on a real display.

Usage: python3 scripts/test_screen_output_failure.py [build/miximus]
Requires an idle local API port, Vulkan presentation, and a display session.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from cef_test_process import PROCESS_CREATION_FLAGS, stop_process

API = "http://127.0.0.1:7351/api/v1"


def windows_for_process(pid):
    import ctypes
    from ctypes import wintypes

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    handles = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(window):
            handles.append(window)
        return True

    user.EnumWindows.argtypes = [type(visit), wintypes.LPARAM]
    if not user.EnumWindows(visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return handles


def resize_windows_output(pid):
    import ctypes
    from ctypes import wintypes

    handles = windows_for_process(pid)
    assert len(handles) == 1, f"Expected one output window, got {handles}"
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT]
    # Resize through the OS, without changing node options or clearing its failure latch.
    if not user.SetWindowPos(handles[0], None, 0, 0, 480, 270, 0x16):
        raise ctypes.WinError(ctypes.get_last_error())
    return handles[0]


def request(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(API + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as response:
        body = response.read()
        return json.loads(body) if body else None


def main():
    try:
        request("/config")
    except (urllib.error.URLError, TimeoutError):
        pass
    else:
        raise RuntimeError("A Miximus instance is already using the test API port")

    default_binary = "build/miximus.exe" if os.name == "nt" else "build/miximus"
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else default_binary).resolve()
    with tempfile.TemporaryDirectory(prefix="miximus-screen-failure-") as directory:
        settings = Path(directory) / "settings.json"
        settings.write_text(json.dumps({"nodes": [
            # Give the presentation worker time to finish its resize stop before the
            # next render frame. This exposes the intentional-stop/failure race.
            {"id": "$app", "type": "application_settings", "schema_version": 2,
             "options": {"frame_rate": {"numerator": 10, "denominator": 1}}},
            {"id": "test-screen", "type": "screen_output", "schema_version": 3, "options": {
            "enabled": True, "fullscreen": True, "monitor_id": "miximus-test-missing-monitor",
            "position": [64, 64], "size": [320, 180]
        }}], "connections": []}))
        log = Path(directory) / "app.log"
        with log.open("w") as output:
            app = subprocess.Popen([str(binary), "--settings", str(settings), "--stop-after", "30"],
                                   stdout=output, stderr=subprocess.STDOUT,
                                   creationflags=PROCESS_CREATION_FLAGS)
            try:
                def status():
                    assert app.poll() is None, "Application exited unexpectedly"
                    return request("/status").get("test-screen", {})

                def wait_for(predicate):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        try:
                            current = status()
                            if predicate(current):
                                return current
                        except (urllib.error.URLError, TimeoutError):
                            pass
                        time.sleep(0.1)
                    raise AssertionError(f"Timed out waiting for screen status: {status()}")

                failure = wait_for(lambda s: s.get("connected") is False and "unavailable" in s.get("screen_error", ""))
                # The graph continues evaluating while the endpoint stays failed.
                for _ in range(20):
                    time.sleep(0.1)
                    current = status()
                    assert current["connected"] is False
                    assert current["screen_error"] == failure["screen_error"]
                request("/control", {"action": "command", "topic": "update_node", "id": "test-screen",
                                     "options": {"fullscreen": False}})
                wait_for(lambda s: s.get("connected") is True and s.get("screen_error") == "" and s.get("swaps_completed", 0) > 0)
                if os.name == "nt":
                    old_window = resize_windows_output(app.pid)
                    wait_for(lambda s: old_window not in windows_for_process(app.pid)
                             and s.get("connected") is True and s.get("screen_error") == ""
                             and s.get("swaps_completed", 0) > 2)
                request("/control", {"action": "command", "topic": "update_node", "id": "test-screen",
                                     "options": {"enabled": False}})
                wait_for(lambda s: s.get("connected") is False and s.get("screen_error") == "")
            except BaseException:
                print(log.read_text(), file=sys.stderr)
                raise
            finally:
                stop_process(app, timeout=20)
            assert app.returncode == 0, f"Application exit: {app.returncode}"
            diagnostics = log.read_text()
            assert "Vulkan validation:" not in diagnostics, diagnostics
    print("PASS: unavailable monitor reported, failure retained, explicit reconfiguration presents, "
          "Windows OS resize recovers when applicable, disable disconnects")


if __name__ == "__main__":
    main()
